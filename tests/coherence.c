/*
 * Minimal sufficient CDM barrier mask for ONE dependency pattern.
 *
 * agx_cdm_barrier() sets all 23 bits of the barrier word after every launch,
 * under a comment saying the bits are not understood and this is "to be safe".
 * That is what stops consecutive dispatches overlapping, which costs ~53x on a
 * workload shaped like Ghost of Tsushima's (many small dispatches, ~213
 * invocations each, on a GPU needing ~4096 threads to hide a 428-905 ns
 * dependent load).
 *
 * Sweeping masks against the whole game gives one opaque data point per ten
 * minutes. This instead hammers a single pattern -- compute writes a buffer,
 * compute reads it, no other traffic, no Vulkan barrier between them -- and
 * asks which bits are actually required for the consumer to observe the
 * producer's writes. It cannot recover what the bits MEAN, but a minimal
 * sufficient mask for this case is real data.
 *
 * Each mask is hammered many times: a coherency failure is a race, so one
 * passing run proves nothing.
 *
 * Build: cc -O2 -o coherence coherence.c -lvulkan
 * Run:   ./coherence [iterations]      (needs HK_PERFTEST=overlap to matter)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "%s:%d: %s -> %d\n", __FILE__, __LINE__, #x, _r); exit(2);} } while (0)

static VkDevice dev; static VkQueue q; static VkCommandPool cp;
static VkCommandBuffer cb; static VkPipeline pipe; static VkPipelineLayout pl;
static VkDescriptorSet ds; static uint32_t *map;

#define N 8192u

struct pc_t { uint32_t mode, n, seed, pad; };

/* producer then consumer, back to back, no barrier. returns mismatches */
static int trial(uint32_t seed)
{
   memset(map, 0, 2 * N * sizeof(uint32_t));

   CHECK(vkResetCommandPool(dev, cp, 0));
   CHECK(vkBeginCommandBuffer(cb, &(VkCommandBufferBeginInfo){
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}));
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);

   struct pc_t a = {.mode = 0, .n = N, .seed = seed};
   vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(a), &a);
   vkCmdDispatch(cb, N / 64, 1, 1);

   struct pc_t b = {.mode = 1, .n = N, .seed = seed};
   vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(b), &b);
   vkCmdDispatch(cb, N / 64, 1, 1);

   CHECK(vkEndCommandBuffer(cb));
   CHECK(vkQueueSubmit(q, 1, &(VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1, .pCommandBuffers = &cb}, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(q));

   int bad = 0;
   for (uint32_t i = 0; i < N; i++) {
      uint32_t want = (i * 2654435761u + seed) + 1u;
      if (map[N + i] != want) bad++;
   }
   return bad;
}

int main(int argc, char **argv)
{
   int iters = argc > 1 ? atoi(argv[1]) : 200;

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
   CHECK(vkCreateDevice(pd, &(VkDeviceCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
        .queueCount = 1, .pQueuePriorities = &prio}}, NULL, &dev));
   vkGetDeviceQueue(dev, 0, 0, &q);

   VkDeviceSize size = 2 * N * sizeof(uint32_t);
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

   FILE *f = fopen("coherence.spv", "rb");
   if (!f) { perror("coherence.spv"); return 2; }
   fseek(f, 0, SEEK_END); size_t sl = ftell(f); fseek(f, 0, SEEK_SET);
   uint32_t *code = malloc(sl);
   if (fread(code, 1, sl, f) != sl) { perror("read"); return 2; }
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
   CHECK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0}, NULL, &cp));
   CHECK(vkAllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1}, &cb));

   int worst = 0, failing = 0;
   for (int i = 0; i < iters; i++) {
      int bad = trial(0x1000u + (uint32_t)i);
      if (bad) { failing++; if (bad > worst) worst = bad; }
   }
   printf("%s  %d/%d trials wrong, worst %d/%u elements\n",
          failing ? "FAIL" : "PASS", failing, iters, worst, N);
   return failing ? 1 : 0;
}
