/*
 * Does a big preamble cost more than it saves on a SMALL dispatch?
 *
 * AGX runs a shader's preamble on a single thread before the body, filling
 * uniform registers. nir_opt_preamble hoists a load there whenever the benefit
 * beats the rewrite cost -- the implicit assumption being that the work is
 * amortised across many invocations.
 *
 * Ghost of Tsushima's hottest compute shader breaks that assumption: its
 * preamble is BIGGER than its body (2976 instructions against 2196, with 199
 * memory loads), and it is dispatched with only 843 invocations -- 13
 * workgroups. If the preamble is a serial chain with no latency hiding, a
 * dispatch that small may pay more for it than it saves.
 *
 * AGX_MESA_DEBUG=nopreamble answers the question globally, and answers it
 * "preambles are a big win" -- but globally is the wrong question, because it
 * also strips the full-screen passes where hoisting obviously pays. This
 * isolates the case that matters.
 *
 * Method: a shader with 96 uniform loads (all hoistable) and a trivial body.
 * Sweep the workgroup count. If the preamble is cheap or well amortised, time
 * is roughly flat until the body starts to matter. If it dominates small
 * dispatches, one workgroup costs nearly as much as many.
 *
 * Build: cc -O2 -o preambletest preambletest.c -lvulkan
 * Run:   ./preambletest            and    AGX_MESA_DEBUG=nopreamble ./preambletest
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "%s:%d: %s -> %d\n", __FILE__, __LINE__, #x, _r); exit(2);} } while (0)

#define UBOV 96u
#define MAXWG 256u

static VkDevice dev; static VkQueue q;

static uint32_t
pick_mem(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want)
{
   VkPhysicalDeviceMemoryProperties mp;
   vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
      if ((bits & (1u << i)) &&
          (mp.memoryTypes[i].propertyFlags & want) == want)
         return i;
   }
   fprintf(stderr, "no memory type\n"); exit(2);
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

   float prio = 1.0f;
   CHECK(vkCreateDevice(pd, &(VkDeviceCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
        .queueCount = 1, .pQueuePriorities = &prio}}, NULL, &dev));
   vkGetDeviceQueue(dev, 0, 0, &q);

   /* SSBO for output, UBO for the hoistable loads */
   VkBuffer ssbo, ubo; VkDeviceMemory smem, umem;
   VkDeviceSize ssz = MAXWG * 64 * sizeof(uint32_t), usz = UBOV * 16;

   CHECK(vkCreateBuffer(dev, &(VkBufferCreateInfo){.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = ssz, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT}, NULL, &ssbo));
   CHECK(vkCreateBuffer(dev, &(VkBufferCreateInfo){.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = usz, .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT}, NULL, &ubo));

   VkMemoryRequirements smr, umr;
   vkGetBufferMemoryRequirements(dev, ssbo, &smr);
   vkGetBufferMemoryRequirements(dev, ubo, &umr);
   VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = smr.size, .memoryTypeIndex = pick_mem(pd, smr.memoryTypeBits, host)}, NULL, &smem));
   CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = umr.size, .memoryTypeIndex = pick_mem(pd, umr.memoryTypeBits, host)}, NULL, &umem));
   CHECK(vkBindBufferMemory(dev, ssbo, smem, 0));
   CHECK(vkBindBufferMemory(dev, ubo, umem, 0));
   void *umap; CHECK(vkMapMemory(dev, umem, 0, usz, 0, &umap));
   memset(umap, 1, usz);

   VkDescriptorSetLayoutBinding bnd[2] = {
      {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
      {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
       .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
   VkDescriptorSetLayout dsl;
   CHECK(vkCreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 2, .pBindings = bnd}, NULL, &dsl));
   VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = 4};
   VkPipelineLayout pl;
   CHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
      .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr}, NULL, &pl));

   FILE *f = fopen("preambletest.spv", "rb");
   if (!f) { perror("preambletest.spv"); return 2; }
   fseek(f, 0, SEEK_END); size_t sl = ftell(f); fseek(f, 0, SEEK_SET);
   uint32_t *code = malloc(sl);
   if (fread(code, 1, sl, f) != sl) { perror("read"); return 2; }
   fclose(f);
   VkShaderModule sm;
   CHECK(vkCreateShaderModule(dev, &(VkShaderModuleCreateInfo){
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sl, .pCode = code}, NULL, &sm));
   VkPipeline pipe;
   CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &(VkComputePipelineCreateInfo){
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main"},
      .layout = pl}, NULL, &pipe));

   VkDescriptorPoolSize ps[2] = {
      {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1},
      {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1}};
   VkDescriptorPool dp;
   CHECK(vkCreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo){
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
      .poolSizeCount = 2, .pPoolSizes = ps}, NULL, &dp));
   VkDescriptorSet ds;
   CHECK(vkAllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo){
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp,
      .descriptorSetCount = 1, .pSetLayouts = &dsl}, &ds));
   VkWriteDescriptorSet w[2] = {
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0,
       .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       .pBufferInfo = &(VkDescriptorBufferInfo){.buffer = ssbo, .range = VK_WHOLE_SIZE}},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1,
       .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
       .pBufferInfo = &(VkDescriptorBufferInfo){.buffer = ubo, .range = VK_WHOLE_SIZE}}};
   vkUpdateDescriptorSets(dev, 2, w, 0, NULL);

   VkQueryPool qp;
   CHECK(vkCreateQueryPool(dev, &(VkQueryPoolCreateInfo){
      .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
      .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2}, NULL, &qp));

   VkCommandPool cp; VkCommandBuffer cb;
   CHECK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0}, NULL, &cp));
   CHECK(vkAllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1}, &cb));

   printf("device: %s\n", props.deviceName);
   printf("%10s %12s %14s\n", "workgroups", "gpu us", "us/workgroup");

   /* 200 dispatches back to back, so one measurement covers many preambles. */
   const unsigned REPS = 200;
   for (unsigned wg = 1; wg <= MAXWG; wg *= 4) {
      double best = 1e30;
      for (int t = 0; t < 5; t++) {
         CHECK(vkResetCommandPool(dev, cp, 0));
         CHECK(vkBeginCommandBuffer(cb, &(VkCommandBufferBeginInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}));
         vkCmdResetQueryPool(cb, qp, 0, 2);
         vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
         vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
         uint32_t nn = wg * 64;
         vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &nn);
         vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, qp, 0);
         for (unsigned r = 0; r < REPS; r++)
            vkCmdDispatch(cb, wg, 1, 1);
         vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, qp, 1);
         CHECK(vkEndCommandBuffer(cb));
         CHECK(vkQueueSubmit(q, 1, &(VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &cb}, VK_NULL_HANDLE));
         CHECK(vkQueueWaitIdle(q));
         uint64_t ts[2] = {0, 0};
         CHECK(vkGetQueryPoolResults(dev, qp, 0, 2, sizeof(ts), ts, sizeof(uint64_t),
                                     VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
         double us = (double)(ts[1] - ts[0]) * props.limits.timestampPeriod / 1000.0 / REPS;
         if (us < best) best = us;
      }
      printf("%10u %12.3f %14.4f\n", wg, best, best / wg);
   }
   return 0;
}
