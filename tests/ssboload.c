/*
 * Does nir_opt_large_constants actually work on this driver, and does it give
 * the right answers?
 *
 * The shader holds a 512-entry constant table in a local array and reads it
 * through a dynamic index -- the shape the pass exists to fix, and the shape
 * Ghost of Tsushima's hottest compute shader has. Without the pass every
 * invocation stores the whole table into its own scratch before reading it.
 *
 * Correctness is checked against the same table computed on the CPU, for every
 * invocation, because "it got faster" is worthless if it got faster by
 * reading the wrong memory -- and a constant-data section that is never
 * uploaded would read plausible-looking garbage.
 *
 * Build: cc -O2 -o bigconst bigconst.c -lvulkan
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "%s:%d: %s -> %d\n", __FILE__, __LINE__, #x, _r); exit(2);} } while (0)

#define N     8192u   /* invocations */
#define TBL    512u   /* table entries, must match bigconst.comp */

int main(int argc, char **argv)
{
   uint32_t shift = argc > 1 ? (uint32_t)atoi(argv[1]) : 3;

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
   printf("device: %s\n", props.deviceName);

   float prio = 1.0f;
   VkDevice dev;
   /* vkd3d-proton enables robustBufferAccess, which is what makes hk lower
    * every dynamically indexed SSBO load with a bounds check. Without it the
    * loads compile bare and this test measures nothing. */
   VkPhysicalDeviceFeatures feats = {.robustBufferAccess = VK_TRUE};
   CHECK(vkCreateDevice(pd, &(VkDeviceCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pEnabledFeatures = &feats,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
        .queueCount = 1, .pQueuePriorities = &prio}}, NULL, &dev));
   VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);

   VkDeviceSize size = N * sizeof(uint32_t);
   VkBuffer buf;
   CHECK(vkCreateBuffer(dev, &(VkBufferCreateInfo){.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT}, NULL, &buf));
   VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, buf, &mr);
   VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   uint32_t mt = UINT32_MAX;
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
      VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
      if ((mr.memoryTypeBits & (1u << i)) && (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
          (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { mt = i; break; } }
   if (mt == UINT32_MAX) { fprintf(stderr, "no UMA memory type\n"); return 2; }
   VkDeviceMemory mem;
   CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = mr.size, .memoryTypeIndex = mt}, NULL, &mem));
   CHECK(vkBindBufferMemory(dev, buf, mem, 0));
   uint32_t *map;
   CHECK(vkMapMemory(dev, mem, 0, size, 0, (void **)&map));

   VkDescriptorSetLayoutBinding bnd = {.binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
   VkDescriptorSetLayout dsl;
   CHECK(vkCreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1, .pBindings = &bnd}, NULL, &dsl));
   VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = 8};
   VkPipelineLayout pl;
   CHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
      .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr}, NULL, &pl));

   FILE *f = fopen("ssboload.spv", "rb");
   if (!f) { perror("ssboload.spv"); return 2; }
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

   VkDescriptorPoolSize ps = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1};
   VkDescriptorPool dp;
   CHECK(vkCreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo){
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
      .poolSizeCount = 1, .pPoolSizes = &ps}, NULL, &dp));
   VkDescriptorSet ds;
   CHECK(vkAllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo){
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp,
      .descriptorSetCount = 1, .pSetLayouts = &dsl}, &ds));
   vkUpdateDescriptorSets(dev, 1, &(VkWriteDescriptorSet){
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0,
      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .pBufferInfo = &(VkDescriptorBufferInfo){.buffer = buf, .offset = 0,
                                               .range = VK_WHOLE_SIZE}}, 0, NULL);

   VkCommandPool cp; VkCommandBuffer cb;
   CHECK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0}, NULL, &cp));
   CHECK(vkAllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1}, &cb));

   memset(map, 0xff, size);
   struct { uint32_t n, shift; } pc = {N, shift};
   CHECK(vkBeginCommandBuffer(cb, &(VkCommandBufferBeginInfo){
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}));
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
   vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
   vkCmdDispatch(cb, N / 64, 1, 1);
   CHECK(vkEndCommandBuffer(cb));
   CHECK(vkQueueSubmit(q, 1, &(VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1, .pCommandBuffers = &cb}, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(q));

   /* Same table, same indexing, on the CPU. */
   uint32_t tbl[TBL];
   for (uint32_t i = 0; i < TBL; i++) tbl[i] = (uint32_t)(i * 2654435761u);

   unsigned bad = 0; uint32_t first_i = 0, got = 0, want = 0;
   for (uint32_t i = 0; i < N; i++) {
      uint32_t idx = (i * 7u + shift) % TBL;
      uint32_t exp = tbl[idx] + tbl[(idx + 13u) % TBL];
      if (map[i] != exp) {
         if (!bad) { first_i = i; got = map[i]; want = exp; }
         bad++;
      }
   }
   if (bad) {
      printf("FAIL  %u/%u wrong; first at i=%u got 0x%08x want 0x%08x\n",
             bad, N, first_i, got, want);
   } else {
      printf("PASS  %u/%u correct\n", N, N);
   }
   return bad ? 1 : 0;
}
