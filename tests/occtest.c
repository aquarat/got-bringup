/*
 * Correctness harness for the AGX_OCCUPANCY register-allocator knob.
 *
 * Runs a compute shader whose result is a pure function of (invocation index,
 * rounds, seed), reads the buffer back through host-visible memory and checks
 * every element against a CPU reference. A register allocator that spills
 * wrongly will produce wrong numbers, not just a crash, so "it compiled" is
 * not accepted as evidence here.
 *
 * Build: cc -O2 -o occtest occtest.c -lvulkan
 * Run:   AGX_OCCUPANCY=512 ./occtest pressure2.spv [rounds] [seed]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CHECK(x)                                                               \
   do {                                                                        \
      VkResult _r = (x);                                                       \
      if (_r != VK_SUCCESS) {                                                  \
         fprintf(stderr, "%s:%d: %s -> %d\n", __FILE__, __LINE__, #x, _r);     \
         exit(1);                                                              \
      }                                                                        \
   } while (0)

#define INVOCATIONS 1024
#define MAX_N 1024

/* Bit-exact CPU mirror of pressure2.comp / pressure3.comp. N must match the
 * shader's #define N (it is a power of two, used as the wrap mask). */
static uint32_t
reference(uint32_t i, uint32_t rounds, uint32_t seed, uint32_t N,
          bool fixed_strides)
{
   uint32_t a[MAX_N];
   const uint32_t M = N - 1;
   uint32_t s = i * 2654435761u + seed;

   for (uint32_t k = 0; k < N; ++k) {
      s = s * 1664525u + 1013904223u;
      a[k] = s ^ (k * 0x9e3779b9u);
   }

   /* Power-of-two N masks; other N uses modulo, matching the shader. */
   bool pot = (N & M) == 0;
   uint32_t s1, s2, s3;
   if (fixed_strides) {
      /* pressure6.comp: compile-time constant strides. */
      s1 = 37u;
      s2 = 71u;
      s3 = 113u;
   } else if (pot) {
      s1 = (seed | 1u) & M;
      s2 = ((seed >> 8) | 1u) & M;
      s3 = ((seed >> 16) | 1u) & M;
   } else {
      s1 = (seed % (N - 1)) + 1u;
      s2 = ((seed >> 8) % (N - 1)) + 1u;
      s3 = ((seed >> 16) % (N - 1)) + 1u;
   }

   for (uint32_t r = 0; r < rounds; ++r) {
      for (uint32_t k = 0; k < N; ++k) {
         uint32_t x = a[k];
         uint32_t y = a[pot ? ((k + s1) & M) : ((k + s1) % N)];
         uint32_t z = a[pot ? ((k + s2) & M) : ((k + s2) % N)];
         uint32_t w = a[pot ? ((k + s3) & M) : ((k + s3) % N)];
         x += (y ^ (z + w));
         x = (x << 7) | (x >> 25);
         x = x * 2246822519u + 374761393u;
         a[k] = x;
      }
   }

   uint32_t acc = 0u;
   for (uint32_t k = 0; k < N; ++k)
      acc = (acc ^ a[k]) * 2654435761u + k;

   return acc;
}

/* Bit-exact CPU mirror of pressure5.comp (uvec2 slots, N = 64). */
static uint32_t
reference_uvec2(uint32_t i, uint32_t rounds, uint32_t seed)
{
   const uint32_t N = 64, M = 63;
   uint32_t ax[64], ay[64];
   uint32_t s = i * 2654435761u + seed;

   for (uint32_t k = 0; k < N; ++k) {
      s = s * 1664525u + 1013904223u;
      ax[k] = s ^ (k * 0x9e3779b9u);
      s = s * 1664525u + 1013904223u;
      ay[k] = s ^ (k * 0x85ebca6bu);
   }

   uint32_t s1 = (seed | 1u) & M;
   uint32_t s2 = ((seed >> 8) | 1u) & M;
   uint32_t s3 = ((seed >> 16) | 1u) & M;

   for (uint32_t r = 0; r < rounds; ++r) {
      for (uint32_t k = 0; k < N; ++k) {
         uint32_t *a[2] = {ax, ay};
         for (int c = 0; c < 2; ++c) {
            uint32_t x = a[c][k];
            uint32_t y = a[c][(k + s1) & M];
            uint32_t z = a[c][(k + s2) & M];
            uint32_t w = a[c][(k + s3) & M];
            x += (y ^ (z + w));
            x = (x << 7) | (x >> 25);
            x = x * 2246822519u + 374761393u;
            a[c][k] = x;
         }
      }
   }

   uint32_t accx = 0u, accy = 0u;
   for (uint32_t k = 0; k < N; ++k) {
      accx = (accx ^ ax[k]) * 2654435761u + k;
      accy = (accy ^ ay[k]) * 2654435761u + k;
   }

   return accx ^ (accy * 3u);
}

static uint32_t *
load_spv(const char *path, size_t *len)
{
   FILE *f = fopen(path, "rb");
   if (!f) {
      perror(path);
      exit(1);
   }
   fseek(f, 0, SEEK_END);
   *len = ftell(f);
   fseek(f, 0, SEEK_SET);
   uint32_t *buf = malloc(*len);
   if (fread(buf, 1, *len, f) != *len) {
      perror("read");
      exit(1);
   }
   fclose(f);
   return buf;
}

int
main(int argc, char **argv)
{
   const char *spv = (argc > 1) ? argv[1] : "pressure2.spv";
   uint32_t rounds = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 0) : 3;
   uint32_t seed = (argc > 3) ? (uint32_t)strtoul(argv[3], NULL, 0) : 0x12345u;
   /* Must match the shader's N. */
   uint32_t nslots = (argc > 4) ? (uint32_t)strtoul(argv[4], NULL, 0) : 64;
   /* N=0 selects the uvec2 (pressure5.comp) reference. */
   bool uvec2_mode = (nslots == 0);
   if (uvec2_mode)
      nslots = 64;
   /* argv[5] = "fixed" selects the constant-stride (pressure6.comp) variant. */
   bool fixed_strides = (argc > 5) && !strcmp(argv[5], "fixed");

   VkInstance inst;
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .pApplicationName = "occtest",
                            .apiVersion = VK_API_VERSION_1_3};
   CHECK(vkCreateInstance(
      &(VkInstanceCreateInfo){.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                              .pApplicationInfo = &app},
      NULL, &inst));

   uint32_t n = 0;
   vkEnumeratePhysicalDevices(inst, &n, NULL);
   VkPhysicalDevice *pds = calloc(n, sizeof(*pds));
   vkEnumeratePhysicalDevices(inst, &n, pds);
   VkPhysicalDevice pd = pds[0];
   for (uint32_t i = 0; i < n; i++) {
      VkPhysicalDeviceProperties p;
      vkGetPhysicalDeviceProperties(pds[i], &p);
      if (p.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) {
         pd = pds[i];
         break;
      }
   }
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pd, &props);

   float prio = 1.0f;
   VkDevice dev;
   CHECK(vkCreateDevice(
      pd,
      &(VkDeviceCreateInfo){
         .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
         .queueCreateInfoCount = 1,
         .pQueueCreateInfos =
            &(VkDeviceQueueCreateInfo){
               .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
               .queueFamilyIndex = 0,
               .queueCount = 1,
               .pQueuePriorities = &prio}},
      NULL, &dev));
   VkQueue q;
   vkGetDeviceQueue(dev, 0, 0, &q);

   VkDeviceSize size = INVOCATIONS * sizeof(uint32_t);
   VkBuffer buf;
   CHECK(vkCreateBuffer(
      dev,
      &(VkBufferCreateInfo){.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                            .size = size,
                            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT},
      NULL, &buf));
   VkMemoryRequirements mr;
   vkGetBufferMemoryRequirements(dev, buf, &mr);
   VkPhysicalDeviceMemoryProperties mp;
   vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   uint32_t mt = UINT32_MAX;
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
      VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      if ((mr.memoryTypeBits & (1u << i)) &&
          (mp.memoryTypes[i].propertyFlags & want) == want) {
         mt = i;
         break;
      }
   }
   if (mt == UINT32_MAX) {
      fprintf(stderr, "no host-visible memory type\n");
      return 1;
   }
   VkDeviceMemory mem;
   CHECK(vkAllocateMemory(
      dev,
      &(VkMemoryAllocateInfo){.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .allocationSize = mr.size,
                              .memoryTypeIndex = mt},
      NULL, &mem));
   CHECK(vkBindBufferMemory(dev, buf, mem, 0));
   void *map = NULL;
   CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &map));
   /* Poison, so a shader that writes nothing fails rather than passing. */
   memset(map, 0xCD, size);

   VkDescriptorSetLayoutBinding b = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
   VkDescriptorSetLayout dsl;
   CHECK(vkCreateDescriptorSetLayout(
      dev,
      &(VkDescriptorSetLayoutCreateInfo){
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
         .bindingCount = 1,
         .pBindings = &b},
      NULL, &dsl));

   VkPushConstantRange pcr = {
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = 8};
   VkPipelineLayout pl;
   CHECK(vkCreatePipelineLayout(
      dev,
      &(VkPipelineLayoutCreateInfo){
         .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
         .setLayoutCount = 1,
         .pSetLayouts = &dsl,
         .pushConstantRangeCount = 1,
         .pPushConstantRanges = &pcr},
      NULL, &pl));

   size_t spvlen;
   uint32_t *code = load_spv(spv, &spvlen);
   VkShaderModule sm;
   CHECK(vkCreateShaderModule(
      dev,
      &(VkShaderModuleCreateInfo){
         .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
         .codeSize = spvlen,
         .pCode = code},
      NULL, &sm));
   VkPipeline pipe;
   CHECK(vkCreateComputePipelines(
      dev, VK_NULL_HANDLE, 1,
      &(VkComputePipelineCreateInfo){
         .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
         .stage = {.sType =
                      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                   .module = sm,
                   .pName = "main"},
         .layout = pl},
      NULL, &pipe));

   VkDescriptorPoolSize ps = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                              .descriptorCount = 1};
   VkDescriptorPool dp;
   CHECK(vkCreateDescriptorPool(
      dev,
      &(VkDescriptorPoolCreateInfo){
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
         .maxSets = 1,
         .poolSizeCount = 1,
         .pPoolSizes = &ps},
      NULL, &dp));
   VkDescriptorSet ds;
   CHECK(vkAllocateDescriptorSets(
      dev,
      &(VkDescriptorSetAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
         .descriptorPool = dp,
         .descriptorSetCount = 1,
         .pSetLayouts = &dsl},
      &ds));
   vkUpdateDescriptorSets(
      dev, 1,
      &(VkWriteDescriptorSet){
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = ds,
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &(VkDescriptorBufferInfo){
            .buffer = buf, .offset = 0, .range = VK_WHOLE_SIZE}},
      0, NULL);

   VkCommandPool cp;
   CHECK(vkCreateCommandPool(
      dev,
      &(VkCommandPoolCreateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .queueFamilyIndex = 0},
      NULL, &cp));
   VkCommandBuffer cb;
   CHECK(vkAllocateCommandBuffers(
      dev,
      &(VkCommandBufferAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = cp,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1},
      &cb));

   struct {
      uint32_t rounds, seed;
   } pc = {rounds, seed};

   CHECK(vkBeginCommandBuffer(
      cb, &(VkCommandBufferBeginInfo){
             .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
             .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}));
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0,
                           NULL);
   vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, &pc);
   vkCmdDispatch(cb, INVOCATIONS / 32, 1, 1);
   CHECK(vkEndCommandBuffer(cb));
   CHECK(vkQueueSubmit(q, 1,
                       &(VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                       .commandBufferCount = 1,
                                       .pCommandBuffers = &cb},
                       VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(q));

   const uint32_t *got = map;
   unsigned bad = 0;
   uint32_t first_bad_i = 0, first_bad_got = 0, first_bad_exp = 0;
   for (uint32_t i = 0; i < INVOCATIONS; ++i) {
      uint32_t exp = uvec2_mode ? reference_uvec2(i, rounds, seed)
                                : reference(i, rounds, seed, nslots, fixed_strides);
      if (got[i] != exp) {
         if (!bad) {
            first_bad_i = i;
            first_bad_got = got[i];
            first_bad_exp = exp;
         }
         bad++;
      }
   }

   const char *occ = getenv("AGX_OCCUPANCY");
   printf("%-24s occupancy=%-6s N=%-4u%s rounds=%-4u seed=0x%-8x %s",
          props.deviceName, occ ? occ : "(unset)", nslots,
          uvec2_mode ? "x2" : (fixed_strides ? "f " : "  "), rounds, seed,
          bad ? "FAIL" : "PASS");
   if (bad)
      printf("  (%u/%u wrong; first i=%u got=0x%08x want=0x%08x)", bad,
             INVOCATIONS, first_bad_i, first_bad_got, first_bad_exp);
   printf("\n");

   vkDestroyPipeline(dev, pipe, NULL);
   vkDestroyShaderModule(dev, sm, NULL);
   vkDestroyPipelineLayout(dev, pl, NULL);
   vkDestroyDescriptorPool(dev, dp, NULL);
   vkDestroyDescriptorSetLayout(dev, dsl, NULL);
   vkDestroyCommandPool(dev, cp, NULL);
   vkUnmapMemory(dev, mem);
   vkDestroyBuffer(dev, buf, NULL);
   vkFreeMemory(dev, mem, NULL);
   vkDestroyDevice(dev, NULL);
   vkDestroyInstance(inst, NULL);
   return bad ? 1 : 0;
}
