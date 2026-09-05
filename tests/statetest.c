/*
 * Independent-dispatch STATE hazard test.
 *
 * WHY THIS EXISTS
 * ---------------
 * The weak CDM barrier lets independent dispatches overlap. coherence.c covers
 * the DATA hazard (one dispatch reads what another wrote). It does not cover
 * the STATE hazard, and neither does cstest.c, which binds one descriptor set
 * once and then dispatches the same pipeline 64 times -- there is nothing for
 * the hardware to get stale.
 *
 * But the state hazard is the one that actually bit us: mask 0x0 passed every
 * local test and then hung the GPU inside the game, and the difference between
 * the game and the tests is that the game rebinds descriptors, push constants
 * and pipelines between dispatches.
 *
 * So: N dispatches, each with ITS OWN buffer, ITS OWN push constants, and
 * alternating between two pipelines. No dispatch reads another's output, so
 * Vulkan requires no barrier anywhere and any correct mask must pass. A
 * failure means the hardware reused stale descriptor/uniform/program state.
 *
 * This turns "run the game for eight minutes and see if it hangs" into a
 * five-second check. It cannot prove a mask safe -- only the game can do that
 * -- but it can prove one unsafe, cheaply.
 *
 * Build: cc -O2 -o statetest statetest.c -lvulkan
 * Run:   AGX_CDM_BARRIER_MASK=0x80 ./statetest [dispatches] [iterations]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "%s:%d: %s -> %d\n", __FILE__, __LINE__, #x, _r); exit(2);} } while (0)

#define MAXD 128u
#define N 4096u          /* elements per buffer */

static VkDevice dev; static VkQueue q; static VkCommandPool cp;
static VkCommandBuffer cb; static VkPipeline pipe[2]; static VkPipelineLayout pl;
static VkDescriptorSet ds[MAXD]; static uint32_t *map[MAXD];
static unsigned NDISP = 64;

struct pc_t { uint32_t tag, n, mul, pad; };

static VkShaderModule load(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f) { perror(path); exit(2); }
   fseek(f, 0, SEEK_END); size_t sl = ftell(f); fseek(f, 0, SEEK_SET);
   uint32_t *code = malloc(sl);
   if (fread(code, 1, sl, f) != sl) { perror("read"); exit(2); }
   fclose(f);
   VkShaderModule sm;
   CHECK(vkCreateShaderModule(dev, &(VkShaderModuleCreateInfo){
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sl, .pCode = code}, NULL, &sm));
   return sm;
}

/* Returns the number of buffers holding wrong data. */
static int trial(uint32_t seed)
{
   for (unsigned d = 0; d < NDISP; d++)
      memset(map[d], 0xff, N * sizeof(uint32_t));

   CHECK(vkResetCommandPool(dev, cp, 0));
   CHECK(vkBeginCommandBuffer(cb, &(VkCommandBufferBeginInfo){
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}));

   for (unsigned d = 0; d < NDISP; d++) {
      /* Rebind everything every time: pipeline, descriptor set, constants.
         This is the shape the game has and the tests did not. */
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe[d & 1]);
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1,
                              &ds[d], 0, NULL);
      struct pc_t pc = {.tag = seed + d, .n = N, .mul = 2654435761u};
      vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
      vkCmdDispatch(cb, N / 64, 1, 1);
   }

   CHECK(vkEndCommandBuffer(cb));
   CHECK(vkQueueSubmit(q, 1, &(VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1, .pCommandBuffers = &cb}, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(q));

   int bad = 0;
   for (unsigned d = 0; d < NDISP; d++) {
      uint32_t tag = seed + d;
      for (uint32_t i = 0; i < N; i++) {
         if (map[d][i] != tag * 2654435761u + i) { bad++; break; }
      }
   }
   return bad;
}

int main(int argc, char **argv)
{
   if (argc > 1) NDISP = atoi(argv[1]);
   if (NDISP > MAXD) NDISP = MAXD;
   int iters = argc > 2 ? atoi(argv[2]) : 200;

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

   VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   VkDeviceSize size = N * sizeof(uint32_t);

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

   VkShaderModule sm[2] = { load("statetest.spv"), load("statetest2.spv") };
   for (int i = 0; i < 2; i++)
      CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &(VkComputePipelineCreateInfo){
         .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
         .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm[i], .pName = "main"},
         .layout = pl}, NULL, &pipe[i]));

   VkDescriptorPoolSize ps = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = MAXD};
   VkDescriptorPool dp;
   CHECK(vkCreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo){
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = MAXD,
      .poolSizeCount = 1, .pPoolSizes = &ps}, NULL, &dp));

   for (unsigned d = 0; d < NDISP; d++) {
      VkBuffer buf;
      CHECK(vkCreateBuffer(dev, &(VkBufferCreateInfo){.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = size, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT}, NULL, &buf));
      VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, buf, &mr);
      uint32_t mt = UINT32_MAX;
      for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
         VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
         if ((mr.memoryTypeBits & (1u << i)) && (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
             (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
             (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { mt = i; break; } }
      if (mt == UINT32_MAX) { fprintf(stderr, "no UMA memory type\n"); return 2; }
      VkDeviceMemory mem;
      CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = mr.size, .memoryTypeIndex = mt}, NULL, &mem));
      CHECK(vkBindBufferMemory(dev, buf, mem, 0));
      CHECK(vkMapMemory(dev, mem, 0, size, 0, (void **)&map[d]));

      CHECK(vkAllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp,
         .descriptorSetCount = 1, .pSetLayouts = &dsl}, &ds[d]));
      vkUpdateDescriptorSets(dev, 1, &(VkWriteDescriptorSet){
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds[d], .dstBinding = 0,
         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &(VkDescriptorBufferInfo){.buffer = buf, .offset = 0,
                                                  .range = VK_WHOLE_SIZE}}, 0, NULL);
   }

   CHECK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0}, NULL, &cp));
   CHECK(vkAllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1}, &cb));

   int worst = 0, failing = 0;
   for (int i = 0; i < iters; i++) {
      int bad = trial(0x1000u + (uint32_t)i * 977u);
      if (bad) { failing++; if (bad > worst) worst = bad; }
   }
   printf("%s  %d/%d trials wrong, worst %d/%u buffers\n",
          failing ? "FAIL" : "PASS", failing, iters, worst, NDISP);
   return failing ? 1 : 0;
}
