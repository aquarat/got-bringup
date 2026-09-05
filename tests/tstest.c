/*
 * Does vkCmdWriteTimestamp2 measure anything on Honeykrisp?
 *
 * WHY
 * Ghost of Tsushima ships a dynamic resolution controller, has it enabled with
 * a 30 fps target and permission to drop to a quarter of the pixels, runs at
 * 5.8 fps, and never scales down. A controller like that decides using GPU
 * frame time from timestamp queries. Reading hk's implementation suggests those
 * queries return a delta of ~0 for work that is purely compute, because the
 * value written is the completion time of the ENCLOSING control stream and the
 * stream is not split until the NEXT write -- so the work lands inside the
 * first timestamp's stream and the second timestamp gets an empty one.
 *
 * A controller told "the GPU took 0 ms" concludes it has unlimited headroom.
 *
 * THE TEST
 * Bracket a large vkCmdFillBuffer -- which hk implements as a compute dispatch
 * -- between two timestamps, with no render pass anywhere. Compare the query
 * delta against the wall-clock time of the submit. If the GPU visibly takes
 * milliseconds and the queries report ~0, the mechanism is confirmed.
 *
 * Build: cc -O2 -o tstest tstest.c -lvulkan
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#define CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "%s:%d: %s failed: %d\n", __FILE__, __LINE__, #x, _r); \
    exit(1);} } while (0)

static double now_ms(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec * 1000.0 + t.tv_nsec / 1.0e6;
}

int main(int argc, char **argv)
{
   /* Big enough that the fill is unmistakably milliseconds of GPU work. */
   VkDeviceSize size = (argc > 1) ? strtoull(argv[1], NULL, 0)
                                  : (VkDeviceSize)512 * 1024 * 1024;

   VkInstance inst;
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .apiVersion = VK_API_VERSION_1_3};
   CHECK(vkCreateInstance(&(VkInstanceCreateInfo){
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &app}, NULL, &inst));

   /* There may be several ICDs loaded (lavapipe, for one); pick the Asahi GPU
    * explicitly rather than whichever happens to enumerate first. */
   uint32_t n = 0;
   vkEnumeratePhysicalDevices(inst, &n, NULL);
   VkPhysicalDevice *pds = calloc(n, sizeof(*pds));
   vkEnumeratePhysicalDevices(inst, &n, pds);
   VkPhysicalDevice pd = VK_NULL_HANDLE;
   for (uint32_t i = 0; i < n; i++) {
      VkPhysicalDeviceProperties p;
      vkGetPhysicalDeviceProperties(pds[i], &p);
      printf("  [%u] %s\n", i, p.deviceName);
      if (pd == VK_NULL_HANDLE && p.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU)
         pd = pds[i];
   }
   if (pd == VK_NULL_HANDLE) pd = pds[0];

   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pd, &props);
   printf("device: %s\n", props.deviceName);
   printf("timestampPeriod: %g ns/tick   validBits: ", props.limits.timestampPeriod);

   uint32_t qn = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
   VkQueueFamilyProperties *qf = calloc(qn, sizeof(*qf));
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf);
   printf("%u (family 0)\n\n", qf[0].timestampValidBits);

   float prio = 1.0f;
   VkDevice dev;
   CHECK(vkCreateDevice(pd, &(VkDeviceCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
               .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
               .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio}},
         NULL, &dev));

   VkQueue q;
   vkGetDeviceQueue(dev, 0, 0, &q);

   /* A device-local buffer to fill. */
   VkBuffer buf;
   CHECK(vkCreateBuffer(dev, &(VkBufferCreateInfo){
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE}, NULL, &buf));

   VkMemoryRequirements mr;
   vkGetBufferMemoryRequirements(dev, buf, &mr);
   VkPhysicalDeviceMemoryProperties mp;
   vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   uint32_t mt = UINT32_MAX;
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((mr.memoryTypeBits & (1u << i)) &&
          (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
         mt = i; break;
      }
   if (mt == UINT32_MAX) { fprintf(stderr, "no device-local memory\n"); return 1; }

   VkDeviceMemory mem;
   CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size, .memoryTypeIndex = mt}, NULL, &mem));
   CHECK(vkBindBufferMemory(dev, buf, mem, 0));

   VkQueryPool pool;
   CHECK(vkCreateQueryPool(dev, &(VkQueryPoolCreateInfo){
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2}, NULL, &pool));

   VkCommandPool cp;
   CHECK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .queueFamilyIndex = 0}, NULL, &cp));
   VkCommandBuffer cb;
   CHECK(vkAllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1}, &cb));

   printf("filling %.0f MiB with vkCmdFillBuffer, bracketed by timestamps\n",
          size / 1048576.0);
   printf("(no render pass anywhere -- this is the pure-compute path)\n\n");

   for (int iter = 0; iter < 3; iter++) {
      CHECK(vkResetCommandPool(dev, cp, 0));
      CHECK(vkBeginCommandBuffer(cb, &(VkCommandBufferBeginInfo){
               .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
               .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}));

      vkCmdResetQueryPool(cb, pool, 0, 2);
      vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, pool, 0);

      /* Several fills so the GPU is unambiguously busy for milliseconds. */
      for (int i = 0; i < 4; i++)
         vkCmdFillBuffer(cb, buf, 0, size, 0xdeadbeefu + i);

      vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, pool, 1);
      CHECK(vkEndCommandBuffer(cb));

      double t0 = now_ms();
      CHECK(vkQueueSubmit(q, 1, &(VkSubmitInfo){
               .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
               .commandBufferCount = 1, .pCommandBuffers = &cb}, VK_NULL_HANDLE));
      CHECK(vkQueueWaitIdle(q));
      double wall = now_ms() - t0;

      uint64_t ts[2] = {0, 0};
      VkResult r = vkGetQueryPoolResults(dev, pool, 0, 2, sizeof(ts), ts,
                                         sizeof(uint64_t),
                                         VK_QUERY_RESULT_64_BIT |
                                         VK_QUERY_RESULT_WAIT_BIT);
      if (r != VK_SUCCESS) { printf("  query result: %d\n", r); continue; }

      double gpu_ms = (double)(ts[1] - ts[0]) * props.limits.timestampPeriod / 1.0e6;
      printf("  iter %d: wall %8.3f ms   timestamps report %8.3f ms   (delta %llu ticks)\n",
             iter, wall, gpu_ms, (unsigned long long)(ts[1] - ts[0]));
   }

   printf("\nIf the reported GPU time is ~0 while the wall time is milliseconds,\n"
          "vkCmdWriteTimestamp2 cannot measure compute work on this driver.\n");
   return 0;
}
