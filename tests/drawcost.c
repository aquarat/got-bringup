/*
 * What does a draw cost, before it draws anything?
 *
 * Ghost of Tsushima issues 6886 draws per frame and spends 7.8 ms per frame in
 * the vertex/tiler phase -- 1.13 us per draw. That is either the hardware's
 * inherent per-draw cost, in which case only the application can help, or it
 * is overhead honeykrisp adds, in which case it is worth attacking.
 *
 * So: one triangle, no vertex inputs, no varyings, a fragment shader that
 * writes a constant, into a small attachment. Whatever this costs is per-draw
 * overhead rather than shading. Sweep the draw count in one render pass and
 * take the slope; the intercept is the render pass, the slope is the draw.
 *
 * Build: cc -O2 -o drawcost drawcost.c -lvulkan
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "%s:%d: %s -> %d\n", __FILE__, __LINE__, #x, _r); exit(2);} } while (0)

#define DIM 64u

static VkShaderModule load(VkDevice dev, const char *p)
{
   FILE *f = fopen(p, "rb");
   if (!f) { perror(p); exit(2); }
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
   VkPhysicalDeviceVulkan13Features f13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .dynamicRendering = VK_TRUE};
   VkDevice dev;
   CHECK(vkCreateDevice(pd, &(VkDeviceCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &f13,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
        .queueCount = 1, .pQueuePriorities = &prio}}, NULL, &dev));
   VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);

   /* Colour attachment */
   VkImage img; VkDeviceMemory imem; VkImageView view;
   CHECK(vkCreateImage(dev, &(VkImageCreateInfo){
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R32_UINT, .extent = {DIM, DIM, 1}, .mipLevels = 1,
      .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED}, NULL, &img));
   VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
   VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   uint32_t mt = UINT32_MAX;
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((mr.memoryTypeBits & (1u << i)) &&
          (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { mt = i; break; }
   CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = mr.size, .memoryTypeIndex = mt}, NULL, &imem));
   CHECK(vkBindImageMemory(dev, img, imem, 0));
   CHECK(vkCreateImageView(dev, &(VkImageViewCreateInfo){
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
      .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R32_UINT,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}}, NULL, &view));

   VkPipelineLayout pl;
   CHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}, NULL, &pl));

   VkShaderModule vs = load(dev, "drawcost.vert.spv"), fs = load(dev, "drawcost.frag.spv");
   VkFormat cfmt = VK_FORMAT_R32_UINT;
   VkPipelineShaderStageCreateInfo stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
   VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
   VkViewport vp = {0, 0, DIM, DIM, 0, 1};
   VkRect2D sc = {{0, 0}, {DIM, DIM}};
   VkPipeline pipe;
   CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &(VkGraphicsPipelineCreateInfo){
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &(VkPipelineRenderingCreateInfo){
         .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
         .colorAttachmentCount = 1, .pColorAttachmentFormats = &cfmt},
      .stageCount = 2, .pStages = stages,
      .pVertexInputState = &(VkPipelineVertexInputStateCreateInfo){
         .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO},
      .pInputAssemblyState = &(VkPipelineInputAssemblyStateCreateInfo){
         .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
         .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST},
      .pViewportState = &(VkPipelineViewportStateCreateInfo){
         .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
         .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc},
      .pRasterizationState = &(VkPipelineRasterizationStateCreateInfo){
         .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
         .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
         .lineWidth = 1.0f},
      .pMultisampleState = &(VkPipelineMultisampleStateCreateInfo){
         .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
         .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT},
      .pColorBlendState = &(VkPipelineColorBlendStateCreateInfo){
         .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
         .attachmentCount = 1, .pAttachments = &cba},
      .layout = pl}, NULL, &pipe));

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
   printf("%8s %12s %14s\n", "draws", "gpu us", "us/draw");

   double prev_us = 0; unsigned prev_d = 0;
   unsigned pts[] = {1,64,256,512,1024,2048,4096,8192,16384};
   for (unsigned pi = 0; pi < sizeof(pts)/sizeof(pts[0]); pi++) { unsigned draws = pts[pi];
      double best = 1e30;
      for (int t = 0; t < 5; t++) {
         CHECK(vkResetCommandPool(dev, cp, 0));
         CHECK(vkBeginCommandBuffer(cb, &(VkCommandBufferBeginInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}));
         vkCmdResetQueryPool(cb, qp, 0, 2);
         vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1,
            &(VkImageMemoryBarrier){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
               .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .image = img,
               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}});
         vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, qp, 0);
         vkCmdBeginRendering(cb, &(VkRenderingInfo){
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = sc, .layerCount = 1, .colorAttachmentCount = 1,
            .pColorAttachments = &(VkRenderingAttachmentInfo){
               .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
               .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE}});
         vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
         for (unsigned d = 0; d < draws; d++)
            vkCmdDraw(cb, 3, 1, 0, 0);
         vkCmdEndRendering(cb);
         vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, qp, 1);
         CHECK(vkEndCommandBuffer(cb));
         CHECK(vkQueueSubmit(q, 1, &(VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &cb}, VK_NULL_HANDLE));
         CHECK(vkQueueWaitIdle(q));
         uint64_t ts[2] = {0, 0};
         CHECK(vkGetQueryPoolResults(dev, qp, 0, 2, sizeof(ts), ts, sizeof(uint64_t),
                                     VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
         double us = (double)(ts[1] - ts[0]) * props.limits.timestampPeriod / 1000.0;
         if (us < best) best = us;
      }
      double marg = prev_d ? (best - prev_us) / (draws - prev_d) : 0;
      printf("%8u %12.2f %14.4f%s\n", draws, best, best / draws,
             prev_d ? "" : "  (includes render pass setup)");
      if (prev_d) printf("%8s %12s %14.4f  marginal\n", "", "", marg);
      prev_us = best; prev_d = draws;
   }
   return 0;
}
