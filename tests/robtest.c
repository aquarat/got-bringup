/*
 * Does robust buffer access still work?
 *
 * hk_shader.c's check_in_bounds() now handles iadd(amul(index, size), C) --
 * the shape an array behind a header produces -- in elements rather than
 * bytes. That is a change to bounds-checking logic, so it needs a test that
 * an out-of-bounds read returns zero and an in-bounds read does not.
 *
 * The shader reads b.data[idx] for idx sweeping from well inside the buffer to
 * well outside it. Vulkan's robustBufferAccess guarantees the out-of-bounds
 * reads return zero (or, strictly, a value from within the buffer -- but hk
 * returns zero, and returning the WRONG IN-BOUNDS value would be a bug worth
 * catching too, so the test checks for zero).
 *
 * Build: cc -O2 -o robtest robtest.c -lvulkan
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "%s:%d: %s -> %d\n", __FILE__, __LINE__, #x, _r); exit(2);} } while (0)

#define HDR   4u     /* uint hdr[4] before the array */
#define ELEMS 256u   /* uint data[256] */
#define N     1024u  /* invocations: indices 0..1023, so 768 are out of bounds */

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

   float prio = 1.0f;
   VkPhysicalDeviceFeatures feats = {.robustBufferAccess = VK_TRUE};
   VkDevice dev;
   CHECK(vkCreateDevice(pd, &(VkDeviceCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pEnabledFeatures = &feats,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
        .queueCount = 1, .pQueuePriorities = &prio}}, NULL, &dev));
   VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);

   /* src: hdr[4] + data[256].  dst: N results. */
   VkDeviceSize ssz = (HDR + ELEMS) * 4, dsz = N * 4;
   VkBuffer sbuf, dbuf; VkDeviceMemory smem, dmem;
   VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   VkBuffer *bufs[2] = {&sbuf, &dbuf}; VkDeviceMemory *mems[2] = {&smem, &dmem};
   VkDeviceSize szs[2] = {ssz, dsz};
   for (int i = 0; i < 2; i++) {
      CHECK(vkCreateBuffer(dev, &(VkBufferCreateInfo){
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = szs[i],
         .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT}, NULL, bufs[i]));
      VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, *bufs[i], &mr);
      uint32_t mt = UINT32_MAX;
      for (uint32_t k = 0; k < mp.memoryTypeCount; k++)
         if ((mr.memoryTypeBits & (1u << k)) &&
             (mp.memoryTypes[k].propertyFlags & want) == want) { mt = k; break; }
      CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = mr.size, .memoryTypeIndex = mt}, NULL, mems[i]));
      CHECK(vkBindBufferMemory(dev, *bufs[i], *mems[i], 0));
   }
   uint32_t *smap, *dmap;
   CHECK(vkMapMemory(dev, smem, 0, ssz, 0, (void **)&smap));
   CHECK(vkMapMemory(dev, dmem, 0, dsz, 0, (void **)&dmap));
   for (uint32_t i = 0; i < HDR + ELEMS; i++) smap[i] = 0xA0000000u + i;
   memset(dmap, 0xff, dsz);

   VkDescriptorSetLayoutBinding bnd[2] = {
      {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
      {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
   VkDescriptorSetLayout dsl;
   CHECK(vkCreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 2, .pBindings = bnd}, NULL, &dsl));
   VkPipelineLayout pl;
   CHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1, .pSetLayouts = &dsl}, NULL, &pl));

   FILE *f = fopen("robtest.spv", "rb");
   if (!f) { perror("robtest.spv"); return 2; }
   fseek(f, 0, SEEK_END); size_t sl = ftell(f); fseek(f, 0, SEEK_SET);
   uint32_t *code = malloc(sl);
   if (fread(code, 1, sl, f) != sl) { perror("read"); return 2; }
   fclose(f);
   VkShaderModule sm;
   CHECK(vkCreateShaderModule(dev, &(VkShaderModuleCreateInfo){
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sl, .pCode = code}, NULL, &sm));
   VkPipeline pipe;
   CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &(VkComputePipelineCreateInfo){
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main"},
      .layout = pl}, NULL, &pipe));

   VkDescriptorPoolSize ps = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2};
   VkDescriptorPool dp;
   CHECK(vkCreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo){
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
      .poolSizeCount = 1, .pPoolSizes = &ps}, NULL, &dp));
   VkDescriptorSet ds;
   CHECK(vkAllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo){
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp,
      .descriptorSetCount = 1, .pSetLayouts = &dsl}, &ds));
   VkDescriptorBufferInfo bi[2] = {
      {.buffer = sbuf, .range = VK_WHOLE_SIZE}, {.buffer = dbuf, .range = VK_WHOLE_SIZE}};
   VkWriteDescriptorSet w[2] = {
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0,
       .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[0]},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1,
       .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[1]}};
   vkUpdateDescriptorSets(dev, 2, w, 0, NULL);

   VkCommandPool cp; VkCommandBuffer cb;
   CHECK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0}, NULL, &cp));
   CHECK(vkAllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1}, &cb));
   CHECK(vkBeginCommandBuffer(cb, &(VkCommandBufferBeginInfo){
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}));
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
   vkCmdDispatch(cb, N / 64, 1, 1);
   CHECK(vkEndCommandBuffer(cb));
   CHECK(vkQueueSubmit(q, 1, &(VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1, .pCommandBuffers = &cb}, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(q));

   unsigned bad_in = 0, bad_out = 0; uint32_t fi = 0, fg = 0, fw = 0;
   for (uint32_t i = 0; i < N; i++) {
      uint32_t want = (i < ELEMS) ? (0xA0000000u + HDR + i) : 0u;
      if (dmap[i] != want) {
         if (i < ELEMS) { if (!bad_in) { fi = i; fg = dmap[i]; fw = want; } bad_in++; }
         else { if (!bad_out) { fi = i; fg = dmap[i]; fw = want; } bad_out++; }
      }
   }
   printf("in-bounds wrong %u/%u   out-of-bounds wrong %u/%u\n",
          bad_in, ELEMS, bad_out, N - ELEMS);
   if (bad_in || bad_out)
      printf("FAIL  first at idx %u: got 0x%08x want 0x%08x\n", fi, fg, fw);
   else
      printf("PASS  in-bounds reads correct, out-of-bounds reads return zero\n");
   return (bad_in || bad_out) ? 1 : 0;
}
