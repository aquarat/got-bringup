/*
 * What does memory actually cost on this GPU, and how much parallelism does it
 * take to hide it?
 *
 * WHY
 * Everything else has been eliminated by measurement on Ghost of Tsushima: ALU
 * throughput, instruction count (a 53% cut in estimated cycles changed frame
 * time by 0%), pixels, dispatch count, cache flushes, register occupancy,
 * feature level. What is left by subtraction is the memory system, which has
 * never been measured.
 *
 * The hot shader is also strikingly under-parallel: 412.8 dispatches per frame,
 * 88,109 invocations total, i.e. ~213 invocations per dispatch -- about three
 * workgroups of 64 on a 32-core GPU. So the question is not only "how slow is
 * memory" but "how many threads does this machine need before latency stops
 * dominating", and does the game ever supply that many.
 *
 * THREE MEASUREMENTS
 *   latency   -- pointer chase over a random permutation. Each load's address
 *                comes from the previous load, so nothing overlaps within a
 *                thread. Swept over working-set size to expose the hierarchy.
 *   bandwidth -- independent strided loads, high thread count. GB/s achieved.
 *   hiding    -- fixed total work, thread count swept. Shows how throughput
 *                scales with parallelism, which is exactly the axis the game's
 *                tiny dispatches sit at the wrong end of.
 *
 * Build: cc -O2 -o memtest memtest.c -lvulkan
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "%s:%d: %s -> %d\n", __FILE__, __LINE__, #x, _r); exit(1);} } while (0)

static VkDevice dev; static VkQueue q; static VkCommandPool cp;
static VkCommandBuffer cb; static VkQueryPool qp; static VkPipeline pipe;
static VkPipelineLayout pl; static VkDescriptorSet ds; static float period;

struct pc_t { uint32_t iters, mask, mode, pad; };

static double run(uint32_t groups, struct pc_t pc)
{
   CHECK(vkResetCommandPool(dev, cp, 0));
   CHECK(vkBeginCommandBuffer(cb, &(VkCommandBufferBeginInfo){
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}));
   vkCmdResetQueryPool(cb, qp, 0, 2);
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
   vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
   vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, qp, 0);
   vkCmdDispatch(cb, groups, 1, 1);
   vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, qp, 1);
   CHECK(vkEndCommandBuffer(cb));
   CHECK(vkQueueSubmit(q, 1, &(VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1, .pCommandBuffers = &cb}, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(q));
   uint64_t ts[2] = {0, 0};
   CHECK(vkGetQueryPoolResults(dev, qp, 0, 2, sizeof(ts), ts, sizeof(uint64_t),
         VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
   return (double)(ts[1] - ts[0]) * period / 1.0e6;   /* ms */
}

int main(void)
{
   VkInstance inst;
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .pEngineName = "vkd3d", .apiVersion = VK_API_VERSION_1_3};
   CHECK(vkCreateInstance(&(VkInstanceCreateInfo){
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app}, NULL, &inst));
   uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, NULL);
   VkPhysicalDevice *pds = calloc(n, sizeof(*pds));
   vkEnumeratePhysicalDevices(inst, &n, pds);
   VkPhysicalDevice pd = pds[0];
   for (uint32_t i = 0; i < n; i++) { VkPhysicalDeviceProperties p;
      vkGetPhysicalDeviceProperties(pds[i], &p);
      if (p.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) { pd = pds[i]; break; } }
   VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
   period = props.limits.timestampPeriod;
   printf("device: %s\n\n", props.deviceName);

   float prio = 1.0f;
   CHECK(vkCreateDevice(pd, &(VkDeviceCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
        .queueCount = 1, .pQueuePriorities = &prio}}, NULL, &dev));
   vkGetDeviceQueue(dev, 0, 0, &q);

   /* 256 MiB, host-visible: this is a UMA part, so the permutation can be
    * written directly without a staging copy. */
   VkDeviceSize size = 256ull * 1024 * 1024;
   VkBuffer buf;
   CHECK(vkCreateBuffer(dev, &(VkBufferCreateInfo){.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT}, NULL, &buf));
   VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, buf, &mr);
   VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   uint32_t mt = UINT32_MAX;
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
      VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
      if ((mr.memoryTypeBits & (1u << i)) && (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
          (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
         mt = i; break; } }
   if (mt == UINT32_MAX) { fprintf(stderr, "no device-local host-visible memory\n"); return 1; }
   VkDeviceMemory mem;
   CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = mr.size, .memoryTypeIndex = mt}, NULL, &mem));
   CHECK(vkBindBufferMemory(dev, buf, mem, 0));

   uint32_t *map = NULL;
   CHECK(vkMapMemory(dev, mem, 0, size, 0, (void **)&map));

   VkDescriptorSetLayoutBinding b = {.binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
   VkDescriptorSetLayout dsl;
   CHECK(vkCreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1, .pBindings = &b}, NULL, &dsl));
   VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = 16};
   CHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
      .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr}, NULL, &pl));

   FILE *f = fopen("memchase.spv", "rb");
   if (!f) { perror("memchase.spv"); return 1; }
   fseek(f, 0, SEEK_END); size_t sl = ftell(f); fseek(f, 0, SEEK_SET);
   uint32_t *code = malloc(sl);
   if (fread(code, 1, sl, f) != sl) { perror("read"); return 1; }
   fclose(f);
   VkShaderModule sm;
   CHECK(vkCreateShaderModule(dev, &(VkShaderModuleCreateInfo){
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sl, .pCode = code}, NULL, &sm));
   CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &(VkComputePipelineCreateInfo){
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main"},
      .layout = pl}, NULL, &pipe));

   VkDescriptorPoolSize ps = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1};
   VkDescriptorPool dp;
   CHECK(vkCreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo){
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
      .poolSizeCount = 1, .pPoolSizes = &ps}, NULL, &dp));
   CHECK(vkAllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo){
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp,
      .descriptorSetCount = 1, .pSetLayouts = &dsl}, &ds));
   vkUpdateDescriptorSets(dev, 1, &(VkWriteDescriptorSet){
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0,
      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .pBufferInfo = &(VkDescriptorBufferInfo){.buffer = buf, .offset = 0,
                                               .range = VK_WHOLE_SIZE}}, 0, NULL);
   CHECK(vkCreateQueryPool(dev, &(VkQueryPoolCreateInfo){
      .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
      .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2}, NULL, &qp));
   CHECK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0}, NULL, &cp));
   CHECK(vkAllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1}, &cb));

   /* ---- 1. dependent-load latency vs working set ---- */
   printf("=== dependent load latency (pointer chase, 1 workgroup of 64) ===\n");
   printf("%12s %10s %14s\n", "working set", "ns/load", "note");
   uint32_t iters = 2048;
   for (unsigned kb = 16; kb <= 131072; kb *= 4) {
      uint32_t entries = (kb * 1024u) / 4u;
      uint32_t mask = entries - 1;
      /* random permutation with a large stride so consecutive hops miss */
      for (uint32_t i = 0; i < entries; i++) map[i] = i;
      for (uint32_t i = entries - 1; i > 0; i--) {
         uint32_t j = (uint32_t)(((uint64_t)rand() << 15 | rand()) % (i + 1));
         uint32_t t = map[i]; map[i] = map[j]; map[j] = t;
      }
      struct pc_t pc = {.iters = iters, .mask = mask, .mode = 0};
      run(1, pc);                                   /* warm */
      double ms = run(1, pc);
      printf("%9u KiB %10.1f %14s\n", kb, ms * 1.0e6 / iters,
             kb <= 128 ? "cache" : (kb >= 32768 ? "DRAM" : ""));
   }

   /* ---- 2. streaming bandwidth vs parallelism ---- */
   printf("\n=== streaming bandwidth (independent loads) ===\n");
   printf("%10s %12s %12s %12s\n", "workgroups", "threads", "GB/s", "ns/load");
   uint32_t bmask = (64u * 1024 * 1024 / 4) - 1;   /* 64 MiB, well beyond cache */
   for (uint32_t groups = 1; groups <= 4096; groups *= 4) {
      struct pc_t pc = {.iters = 256, .mask = bmask, .mode = 1};
      run(groups, pc);
      double ms = run(groups, pc);
      double loads = (double)groups * 64.0 * 256.0;
      printf("%10u %12.0f %12.1f %12.2f\n", groups, groups * 64.0,
             loads * 4.0 / (ms / 1000.0) / 1e9, ms * 1e6 / loads);
   }

   /* ---- 3. the latency-hiding curve: fixed work, varying parallelism ---- */
   printf("\n=== latency hiding: SAME total dependent loads, more threads ===\n");
   printf("%10s %10s %12s %12s\n", "workgroups", "threads", "total ms", "vs 1 group");
   double base = 0;
   for (uint32_t groups = 1; groups <= 4096; groups *= 4) {
      uint32_t per = 65536 / groups; if (!per) per = 1;
      struct pc_t pc = {.iters = per, .mask = (16u * 1024 * 1024 / 4) - 1, .mode = 0};
      run(groups, pc);
      double ms = run(groups, pc);
      if (groups == 1) base = ms;
      printf("%10u %10.0f %12.3f %11.2fx\n", groups, groups * 64.0, ms, base / ms);
   }
   printf("\nThe game's hot shader dispatches ~213 invocations at a time,\n"
          "i.e. between the first and second rows of the last table.\n");

   /* ---- 4. does the driver overlap INDEPENDENT dispatches? ----
    *
    * The game issues ~412 dispatches of ~213 invocations each per frame. If the
    * driver let independent ones overlap, they would together supply the
    * thousands of threads the table above says are needed. hk passes
    * AGX_BARRIER_ALL for every vkCmdDispatch, emitting a full cache flush after
    * each, so the question is whether that serialises them.
    *
    * Same total work, issued two ways, with NO vkCmdPipelineBarrier between the
    * small dispatches -- so Vulkan permits them to run concurrently. */
   printf("\n=== can independent small dispatches overlap? ===\n");
   printf("%-38s %12s\n", "shape", "ms");
   {
      uint32_t mask = (16u * 1024 * 1024 / 4) - 1;
      struct pc_t pc = {.iters = 512, .mask = mask, .mode = 0};

      /* one dispatch supplying 4096 threads */
      run(64, pc); double big = run(64, pc);
      printf("%-38s %12.3f\n", "1 dispatch x 64 groups (4096 thr)", big);

      /* 64 separate dispatches of 1 group each: same threads, same work */
      CHECK(vkResetCommandPool(dev, cp, 0));
      CHECK(vkBeginCommandBuffer(cb, &(VkCommandBufferBeginInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}));
      vkCmdResetQueryPool(cb, qp, 0, 2);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
      vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
      vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, qp, 0);
      for (int i = 0; i < 64; i++)
         vkCmdDispatch(cb, 1, 1, 1);
      vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, qp, 1);
      CHECK(vkEndCommandBuffer(cb));
      CHECK(vkQueueSubmit(q, 1, &(VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = 1, .pCommandBuffers = &cb}, VK_NULL_HANDLE));
      CHECK(vkQueueWaitIdle(q));
      uint64_t ts[2] = {0, 0};
      CHECK(vkGetQueryPoolResults(dev, qp, 0, 2, sizeof(ts), ts, sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
      double many = (double)(ts[1] - ts[0]) * period / 1.0e6;
      printf("%-38s %12.3f\n", "64 dispatches x 1 group (64 thr each)", many);
      printf("\n  ratio %.2fx. If they overlapped, these would be similar.\n", many / big);
   }
   return 0;
}
