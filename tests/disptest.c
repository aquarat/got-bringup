/*
 * What does one compute dispatch cost on Honeykrisp, independent of its work?
 *
 * WHY
 * Ghost of Tsushima spends 135.9 ms/frame in compute at 1920x1200 and 130.0 ms
 * at 1280x720 -- 60% less work for a 4% saving. What did not change across
 * those two runs is the dispatch count: 1781.6 vs 1788.0 per frame. Compute
 * time is dispatches x ~75 us and barely depends on what is inside them.
 *
 * 75 us is far too much for a launch. The suspect is in hk_cmd_dispatch.c:
 * every dispatch from vkCmdDispatch is issued with AGX_BARRIER_ALL, which emits
 * a full conservative CDM cache flush after it -- draining the GPU between
 * consecutive dispatches that may well be independent.
 *
 * THE TEST
 * Submit N dispatches of identical, trivial work and time the batch with the
 * (now working) timestamp queries. Cost per dispatch is then read straight off.
 * Sweeping the work per dispatch separates the fixed cost from the variable
 * one: if 1 and 64 workgroups cost the same, the cost is not the work.
 *
 * vkCmdFillBuffer is used as the dispatch source because hk implements it as a
 * compute dispatch through exactly the same path, with no shader plumbing here.
 *
 * Build: cc -O2 -o disptest disptest.c -lvulkan
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "%s:%d: %s -> %d\n", __FILE__, __LINE__, #x, _r); \
    exit(1);} } while (0)

static VkDevice dev;
static VkQueue q;
static VkCommandPool cp;
static VkCommandBuffer cb;
static VkQueryPool pool;
static VkBuffer buf;
static float period;

/* n dispatches, each filling bytes_each of the buffer. */
static double run(unsigned n, VkDeviceSize bytes_each)
{
   CHECK(vkResetCommandPool(dev, cp, 0));
   CHECK(vkBeginCommandBuffer(cb, &(VkCommandBufferBeginInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}));
   vkCmdResetQueryPool(cb, pool, 0, 2);
   vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, pool, 0);
   for (unsigned i = 0; i < n; i++)
      vkCmdFillBuffer(cb, buf, 0, bytes_each, 0x11111111u + i);
   vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, pool, 1);
   CHECK(vkEndCommandBuffer(cb));

   CHECK(vkQueueSubmit(q, 1, &(VkSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &cb}, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(q));

   uint64_t ts[2] = {0, 0};
   CHECK(vkGetQueryPoolResults(dev, pool, 0, 2, sizeof(ts), ts, sizeof(uint64_t),
                               VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
   return (double)(ts[1] - ts[0]) * period / 1.0e6;   /* ms */
}

int main(void)
{
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
   period = props.limits.timestampPeriod;
   printf("device: %s   timestampPeriod %g ns\n\n", props.deviceName, period);

   float prio = 1.0f;
   CHECK(vkCreateDevice(pd, &(VkDeviceCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
               .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
               .queueFamilyIndex = 0, .queueCount = 1,
               .pQueuePriorities = &prio}}, NULL, &dev));
   vkGetDeviceQueue(dev, 0, 0, &q);

   VkDeviceSize size = 64u * 1024 * 1024;
   CHECK(vkCreateBuffer(dev, &(VkBufferCreateInfo){
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT}, NULL, &buf));
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

   CHECK(vkCreateQueryPool(dev, &(VkQueryPoolCreateInfo){
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2}, NULL, &pool));
   CHECK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .queueFamilyIndex = 0}, NULL, &cp));
   CHECK(vkAllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1}, &cb));

   run(64, 4096);   /* warm up */

   printf("%10s %12s %12s %12s\n", "bytes/disp", "256 disp ms", "1024 disp ms",
          "us/dispatch");
   VkDeviceSize sizes[] = {256, 4096, 65536, 1048576, 16777216};
   for (unsigned i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
      double a = run(256, sizes[i]);
      double b = run(1024, sizes[i]);
      /* Slope over the extra 768 dispatches removes any fixed submit cost. */
      printf("%10llu %12.3f %12.3f %12.2f\n",
             (unsigned long long)sizes[i], a, b, (b - a) * 1000.0 / 768.0);
   }
   printf("\nIf us/dispatch is flat across four orders of magnitude of work,\n"
          "the cost is the dispatch itself, not what is inside it.\n");
   return 0;
}
