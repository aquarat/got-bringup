/*
 * Graphics-stage counterpart to occtest.c.
 *
 * The compute test cannot cover everything the register allocator's occupancy
 * bound has to respect: compute shaders have preloaded registers but no
 * EXPORTs, and it is exports (fixed register indices that lower_exports()
 * writes after RA has finished) that set the hard floor on how small the
 * register file can be made. So this renders a full-screen triangle with a
 * register-hungry fragment shader, through a vertex shader with eight vec4
 * varyings, into an R32_UINT attachment, and checks every pixel against a CPU
 * reference.
 *
 * Build: cc -O2 -o gfxtest gfxtest.c -lvulkan
 * Run:   AGX_OCCUPANCY=1024 ./gfxtest [rounds] [seed]
 */
#include <stdio.h>
#include <stdlib.h>
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

#define DIM 64
#define N 128
#define S1 37u
#define S2 71u
#define S3 113u

/* Bit-exact CPU mirror of gfxpressure.frag. */
static uint32_t
reference(uint32_t i, uint32_t rounds, uint32_t seed)
{
   uint32_t a[N];
   uint32_t s = i * 2654435761u + seed;

   for (uint32_t k = 0; k < N; ++k) {
      s = s * 1664525u + 1013904223u;
      a[k] = s ^ (k * 0x9e3779b9u);
   }

   for (uint32_t r = 0; r < rounds; ++r) {
      for (uint32_t k = 0; k < N; ++k) {
         uint32_t x =
            a[k] + (a[(k + S1) % N] ^ (a[(k + S2) % N] + a[(k + S3) % N]));
         x = (x << 7) | (x >> 25);
         a[k] = x * 2246822519u + 374761393u;
      }
   }

   uint32_t acc = 0u;
   for (uint32_t k = 0; k < N; ++k)
      acc = (acc ^ a[k]) * 2654435761u + k;

   /* The shader adds sum(v*.w) - 8, and every v*.w is exactly 1.0. */
   return acc;
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

static VkShaderModule
make_module(VkDevice dev, const char *path)
{
   size_t len;
   uint32_t *code = load_spv(path, &len);
   VkShaderModule sm;
   CHECK(vkCreateShaderModule(
      dev,
      &(VkShaderModuleCreateInfo){
         .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
         .codeSize = len,
         .pCode = code},
      NULL, &sm));
   free(code);
   return sm;
}

static uint32_t
find_mem(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want)
{
   VkPhysicalDeviceMemoryProperties mp;
   vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
      if ((bits & (1u << i)) &&
          (mp.memoryTypes[i].propertyFlags & want) == want)
         return i;
   }
   fprintf(stderr, "no memory type with 0x%x\n", want);
   exit(1);
}

int
main(int argc, char **argv)
{
   uint32_t rounds = (argc > 1) ? (uint32_t)strtoul(argv[1], NULL, 0) : 3;
   uint32_t seed = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 0) : 0x12345u;

   VkInstance inst;
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .pApplicationName = "gfxtest",
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
   VkPhysicalDeviceVulkan13Features f13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .dynamicRendering = VK_TRUE,
      .synchronization2 = VK_TRUE};
   VkDevice dev;
   CHECK(vkCreateDevice(
      pd,
      &(VkDeviceCreateInfo){
         .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
         .pNext = &f13,
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

   /* Colour attachment: R32_UINT, so the fragment result is exact. */
   VkImage img;
   CHECK(vkCreateImage(
      dev,
      &(VkImageCreateInfo){.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                           .imageType = VK_IMAGE_TYPE_2D,
                           .format = VK_FORMAT_R32_UINT,
                           .extent = {DIM, DIM, 1},
                           .mipLevels = 1,
                           .arrayLayers = 1,
                           .samples = VK_SAMPLE_COUNT_1_BIT,
                           .tiling = VK_IMAGE_TILING_OPTIMAL,
                           .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED},
      NULL, &img));
   VkMemoryRequirements imr;
   vkGetImageMemoryRequirements(dev, img, &imr);
   VkDeviceMemory imem;
   CHECK(vkAllocateMemory(
      dev,
      &(VkMemoryAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = imr.size,
         .memoryTypeIndex = find_mem(pd, imr.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)},
      NULL, &imem));
   CHECK(vkBindImageMemory(dev, img, imem, 0));

   VkImageView view;
   CHECK(vkCreateImageView(
      dev,
      &(VkImageViewCreateInfo){
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = img,
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = VK_FORMAT_R32_UINT,
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
      NULL, &view));

   /* Readback buffer. */
   VkDeviceSize bsize = DIM * DIM * sizeof(uint32_t);
   VkBuffer rb;
   CHECK(vkCreateBuffer(
      dev,
      &(VkBufferCreateInfo){.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                            .size = bsize,
                            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT},
      NULL, &rb));
   VkMemoryRequirements bmr;
   vkGetBufferMemoryRequirements(dev, rb, &bmr);
   VkDeviceMemory bmem;
   CHECK(vkAllocateMemory(
      dev,
      &(VkMemoryAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = bmr.size,
         .memoryTypeIndex =
            find_mem(pd, bmr.memoryTypeBits,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)},
      NULL, &bmem));
   CHECK(vkBindBufferMemory(dev, rb, bmem, 0));
   void *map = NULL;
   CHECK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, &map));
   memset(map, 0xCD, bsize);

   VkPushConstantRange pcr = {
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = 8};
   VkPipelineLayout pl;
   CHECK(vkCreatePipelineLayout(
      dev,
      &(VkPipelineLayoutCreateInfo){
         .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
         .pushConstantRangeCount = 1,
         .pPushConstantRanges = &pcr},
      NULL, &pl));

   VkShaderModule vs = make_module(dev, "gfxpressure.vert.spv");
   VkShaderModule fs = make_module(dev, "gfxpressure.frag.spv");

   VkFormat cfmt = VK_FORMAT_R32_UINT;
   VkPipelineShaderStageCreateInfo stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT,
       .module = vs,
       .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
       .module = fs,
       .pName = "main"},
   };
   VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
   VkViewport vp = {0, 0, DIM, DIM, 0.0f, 1.0f};
   VkRect2D sc = {{0, 0}, {DIM, DIM}};
   VkPipeline pipe;
   CHECK(vkCreateGraphicsPipelines(
      dev, VK_NULL_HANDLE, 1,
      &(VkGraphicsPipelineCreateInfo){
         .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
         .pNext =
            &(VkPipelineRenderingCreateInfo){
               .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
               .colorAttachmentCount = 1,
               .pColorAttachmentFormats = &cfmt},
         .stageCount = 2,
         .pStages = stages,
         .pVertexInputState =
            &(VkPipelineVertexInputStateCreateInfo){
               .sType =
                  VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO},
         .pInputAssemblyState =
            &(VkPipelineInputAssemblyStateCreateInfo){
               .sType =
                  VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
               .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST},
         .pViewportState =
            &(VkPipelineViewportStateCreateInfo){
               .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
               .viewportCount = 1,
               .pViewports = &vp,
               .scissorCount = 1,
               .pScissors = &sc},
         .pRasterizationState =
            &(VkPipelineRasterizationStateCreateInfo){
               .sType =
                  VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
               .polygonMode = VK_POLYGON_MODE_FILL,
               .cullMode = VK_CULL_MODE_NONE,
               .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
               .lineWidth = 1.0f},
         .pMultisampleState =
            &(VkPipelineMultisampleStateCreateInfo){
               .sType =
                  VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT},
         .pColorBlendState =
            &(VkPipelineColorBlendStateCreateInfo){
               .sType =
                  VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
               .attachmentCount = 1,
               .pAttachments = &cba},
         .layout = pl},
      NULL, &pipe));

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

   vkCmdPipelineBarrier(
      cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1,
      &(VkImageMemoryBarrier){
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .image = img,
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}});

   vkCmdBeginRendering(
      cb, &(VkRenderingInfo){
             .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
             .renderArea = sc,
             .layerCount = 1,
             .colorAttachmentCount = 1,
             .pColorAttachments = &(VkRenderingAttachmentInfo){
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView = view,
                .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue = {.color = {.uint32 = {0xDEADBEEF}}}}});
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8, &pc);
   vkCmdDraw(cb, 3, 1, 0, 0);
   vkCmdEndRendering(cb);

   vkCmdPipelineBarrier(
      cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
      &(VkImageMemoryBarrier){
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         .image = img,
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}});

   vkCmdCopyImageToBuffer(
      cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1,
      &(VkBufferImageCopy){
         .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .imageExtent = {DIM, DIM, 1}});

   CHECK(vkEndCommandBuffer(cb));
   CHECK(vkQueueSubmit(q, 1,
                       &(VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                       .commandBufferCount = 1,
                                       .pCommandBuffers = &cb},
                       VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(q));

   const uint32_t *got = map;
   unsigned bad = 0, first = 0;
   uint32_t fg = 0, fe = 0;
   for (uint32_t i = 0; i < DIM * DIM; ++i) {
      uint32_t exp = reference(i, rounds, seed);
      if (got[i] != exp) {
         if (!bad) {
            first = i;
            fg = got[i];
            fe = exp;
         }
         bad++;
      }
   }

   const char *occ = getenv("AGX_OCCUPANCY");
   printf("%-24s occupancy=%-6s %ux%u rounds=%-4u seed=0x%-8x %s",
          props.deviceName, occ ? occ : "(unset)", DIM, DIM, rounds, seed,
          bad ? "FAIL" : "PASS");
   if (bad)
      printf("  (%u/%u wrong; first px=%u got=0x%08x want=0x%08x)", bad,
             DIM * DIM, first, fg, fe);
   printf("\n");

   vkDestroyPipeline(dev, pipe, NULL);
   vkDestroyPipelineLayout(dev, pl, NULL);
   vkDestroyShaderModule(dev, vs, NULL);
   vkDestroyShaderModule(dev, fs, NULL);
   vkDestroyCommandPool(dev, cp, NULL);
   vkUnmapMemory(dev, bmem);
   vkDestroyBuffer(dev, rb, NULL);
   vkFreeMemory(dev, bmem, NULL);
   vkDestroyImageView(dev, view, NULL);
   vkDestroyImage(dev, img, NULL);
   vkFreeMemory(dev, imem, NULL);
   vkDestroyDevice(dev, NULL);
   vkDestroyInstance(inst, NULL);
   return bad ? 1 : 0;
}
