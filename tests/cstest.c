/*
 * A real vkCmdDispatch harness, so driver changes can be tested in seconds
 * instead of a twelve-minute game run.
 *
 * vkCmdFillBuffer (tests/disptest.c) goes through hk_dispatch_precomp, which is
 * a different path from the application dispatches this investigation is about.
 * HK_GPUTIME_ISOLATE hooks hk_dispatch_with_usc -- the shader path -- so it
 * cannot be exercised by fills at all. Hence this.
 *
 * It also answers the question the game data raised: can a compute shader with
 * a data-dependent loop take milliseconds while doing almost no work? The
 * shader below loops a runtime-supplied number of times, so the answer is
 * measured rather than assumed.
 *
 * Build: see build.sh next to it (needs glslangValidator).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "%s:%d: %s -> %d\n", __FILE__, __LINE__, #x, _r); \
    exit(1);} } while (0)

static uint32_t *load_spv(const char *path, size_t *len)
{
   FILE *f = fopen(path, "rb");
   if (!f) { perror(path); exit(1); }
   fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
   uint32_t *buf = malloc(*len);
   if (fread(buf, 1, *len, f) != *len) { perror("read"); exit(1); }
   fclose(f);
   return buf;
}

int main(int argc, char **argv)
{
   const char *spv = (argc > 1) ? argv[1] : "cstest.spv";
   /* HK_GPUTIME only reports from inside a submit, so a short run can finish
    * before the first report period elapses. Repeat to give it submits. */
   int reps = (argc > 2) ? atoi(argv[2]) : 1;

   VkInstance inst;
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .pEngineName = "vkd3d",
                            .apiVersion = VK_API_VERSION_1_3};
   CHECK(vkCreateInstance(&(VkInstanceCreateInfo){
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &app}, NULL, &inst));

   uint32_t n = 0;
   vkEnumeratePhysicalDevices(inst, &n, NULL);
   VkPhysicalDevice *pds = calloc(n, sizeof(*pds));
   vkEnumeratePhysicalDevices(inst, &n, pds);
   VkPhysicalDevice pd = pds[0];
   for (uint32_t i = 0; i < n; i++) {
      VkPhysicalDeviceProperties p;
      vkGetPhysicalDeviceProperties(pds[i], &p);
      if (p.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) { pd = pds[i]; break; }
   }
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pd, &props);
   printf("device: %s\n\n", props.deviceName);

   float prio = 1.0f;
   VkDevice dev;
   CHECK(vkCreateDevice(pd, &(VkDeviceCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
               .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
               .queueFamilyIndex = 0, .queueCount = 1,
               .pQueuePriorities = &prio}}, NULL, &dev));
   VkQueue q;
   vkGetDeviceQueue(dev, 0, 0, &q);

   /* storage buffer */
   VkDeviceSize size = 16 * 1024 * 1024;
   VkBuffer buf;
   CHECK(vkCreateBuffer(dev, &(VkBufferCreateInfo){
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT}, NULL, &buf));
   VkMemoryRequirements mr;
   vkGetBufferMemoryRequirements(dev, buf, &mr);
   VkPhysicalDeviceMemoryProperties mp;
   vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   uint32_t mt = 0;
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((mr.memoryTypeBits & (1u << i)) &&
          (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
         mt = i; break;
      }
   VkDeviceMemory mem;
   CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size, .memoryTypeIndex = mt}, NULL, &mem));
   CHECK(vkBindBufferMemory(dev, buf, mem, 0));

   VkDescriptorSetLayoutBinding b = {
      .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
   VkDescriptorSetLayout dsl;
   CHECK(vkCreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1, .pBindings = &b}, NULL, &dsl));

   VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                              .offset = 0, .size = 8};
   VkPipelineLayout pl;
   CHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1, .pSetLayouts = &dsl,
            .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr}, NULL, &pl));

   size_t spvlen;
   uint32_t *code = load_spv(spv, &spvlen);
   VkShaderModule sm;
   CHECK(vkCreateShaderModule(dev, &(VkShaderModuleCreateInfo){
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = spvlen, .pCode = code}, NULL, &sm));
   VkPipeline pipe;
   CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1,
            &(VkComputePipelineCreateInfo){
               .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
               .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                         .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                         .module = sm, .pName = "main"},
               .layout = pl}, NULL, &pipe));

   VkDescriptorPoolSize ps = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                              .descriptorCount = 1};
   VkDescriptorPool dp;
   CHECK(vkCreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps}, NULL, &dp));
   VkDescriptorSet ds;
   CHECK(vkAllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo){
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = dp, .descriptorSetCount = 1,
            .pSetLayouts = &dsl}, &ds));
   vkUpdateDescriptorSets(dev, 1, &(VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &(VkDescriptorBufferInfo){
               .buffer = buf, .offset = 0, .range = VK_WHOLE_SIZE}}, 0, NULL);

   VkQueryPool qp;
   CHECK(vkCreateQueryPool(dev, &(VkQueryPoolCreateInfo){
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2}, NULL, &qp));
   VkCommandPool cp;
   CHECK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .queueFamilyIndex = 0}, NULL, &cp));
   VkCommandBuffer cb;
   CHECK(vkAllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1}, &cb));

   struct { uint32_t loops, stride; } pc;

   printf("%10s %10s %10s %12s %14s\n", "dispatches", "groups", "loops",
          "gpu ms", "us/dispatch");
   struct { unsigned disp, groups, loops; } cases[] = {
      {  64,   1,     1 },
      {  64,   1,  1000 },
      {  64,   1, 100000 },
      {  64, 128,     1 },
      {  64, 128,  1000 },
      { 256,   1, 100000 },
   };
   /* CSTEST_CASE=disp,groups,loops runs exactly one case, so a compiler-flag
    * sweep does not have to pay for the whole table. */
   const char *sel = getenv("CSTEST_CASE");
   unsigned ncases = sizeof(cases)/sizeof(cases[0]);
   unsigned d, g, lp;
   if (sel && sscanf(sel, "%u,%u,%u", &d, &g, &lp) == 3) {
      cases[0].disp = d; cases[0].groups = g; cases[0].loops = lp;
      ncases = 1;
   }
   for (int rep = 0; rep < reps; rep++)
   for (unsigned i = 0; i < ncases; i++) {
      pc.loops = cases[i].loops;
      pc.stride = 64;
      CHECK(vkResetCommandPool(dev, cp, 0));
      CHECK(vkBeginCommandBuffer(cb, &(VkCommandBufferBeginInfo){
               .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
               .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}));
      /* The profiler cannot time a command whose timestamp slot the
       * application has already claimed -- hk counts those as "skipped". So to
       * observe the driver's own per-command timing (and whether
       * HK_GPUTIME_ISOLATE splits streams), the test must not take timestamps
       * itself. */
      int use_ts = getenv("CSTEST_NO_TS") == NULL;
      if (use_ts) vkCmdResetQueryPool(cb, qp, 0, 2);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1,
                              &ds, 0, NULL);
      vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, &pc);
      if (use_ts) vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, qp, 0);
      for (unsigned d = 0; d < cases[i].disp; d++)
         vkCmdDispatch(cb, cases[i].groups, 1, 1);
      if (use_ts) vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, qp, 1);
      CHECK(vkEndCommandBuffer(cb));
      CHECK(vkQueueSubmit(q, 1, &(VkSubmitInfo){
               .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
               .commandBufferCount = 1, .pCommandBuffers = &cb}, VK_NULL_HANDLE));
      CHECK(vkQueueWaitIdle(q));
      double ms = 0;
      if (use_ts) {
         uint64_t ts[2] = {0, 0};
         CHECK(vkGetQueryPoolResults(dev, qp, 0, 2, sizeof(ts), ts, sizeof(uint64_t),
                                     VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
         ms = (double)(ts[1] - ts[0]) * props.limits.timestampPeriod / 1.0e6;
      }
      printf("%10u %10u %10u %12.3f %14.2f\n", cases[i].disp, cases[i].groups,
             cases[i].loops, ms, ms * 1000.0 / cases[i].disp);
   }
   return 0;
}
