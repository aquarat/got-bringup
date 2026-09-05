/*
 * Does the firmware's per-command interval measure EXECUTION, or does it
 * include time the command spends waiting for earlier work?
 *
 * WHY THIS MATTERS
 * The per-shader profiler charges a shader with the firmware interval of any
 * control stream that held only its dispatches. On Ghost of Tsushima that
 * attributed 23.67 ms per frame to a shader which, read from its NIR, is a
 * buffer clear: 352 invocations storing zero at a 48-byte stride, no loop, no
 * loads. Roughly 16 KB of writes. It cannot execute for 23 ms. Either the
 * interval includes waiting, or the attribution is wrong.
 *
 * THE TEST
 * Two shaders: one that loops hard, one that stores a handful of words. Submit
 * HEAVY then TRIVIAL, back to back, each in its own control stream. If the
 * trivial shader is charged a large interval only when it follows the heavy
 * one, the interval includes waiting and per-command timing cannot be read as
 * per-shader cost.
 *
 * Run:  HK_GPUTIME=1 ./waittest heavy.spv trivial.spv
 * and read the per-shader measured column in the report.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "%s:%d: %s -> %d\n", __FILE__, __LINE__, #x, _r); exit(1);} } while (0)

static VkDevice dev; static VkQueue q; static VkCommandPool cp;
static VkCommandBuffer cb; static VkQueryPool qp; static float period;

static uint32_t *load_spv(const char *p, size_t *len) {
   FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(1); }
   fseek(f,0,SEEK_END); *len=ftell(f); fseek(f,0,SEEK_SET);
   uint32_t *b = malloc(*len);
   if (fread(b,1,*len,f) != *len) { perror("read"); exit(1); }
   fclose(f); return b;
}

int main(int argc, char **argv)
{
   const char *heavy_spv = argc > 1 ? argv[1] : "cstest.spv";
   VkInstance inst;
   VkApplicationInfo app = {.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .pEngineName="vkd3d", .apiVersion=VK_API_VERSION_1_3};
   CHECK(vkCreateInstance(&(VkInstanceCreateInfo){
      .sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo=&app}, NULL, &inst));
   uint32_t n=0; vkEnumeratePhysicalDevices(inst,&n,NULL);
   VkPhysicalDevice *pds=calloc(n,sizeof(*pds)); vkEnumeratePhysicalDevices(inst,&n,pds);
   VkPhysicalDevice pd=pds[0];
   for (uint32_t i=0;i<n;i++){VkPhysicalDeviceProperties p;vkGetPhysicalDeviceProperties(pds[i],&p);
      if(p.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU){pd=pds[i];break;}}
   VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd,&props);
   period=props.limits.timestampPeriod;
   printf("device: %s\n\n", props.deviceName);

   float prio=1.0f;
   CHECK(vkCreateDevice(pd,&(VkDeviceCreateInfo){.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount=1,.pQueueCreateInfos=&(VkDeviceQueueCreateInfo){
        .sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=0,
        .queueCount=1,.pQueuePriorities=&prio}},NULL,&dev));
   vkGetDeviceQueue(dev,0,0,&q);

   VkDeviceSize size=16*1024*1024; VkBuffer buf;
   CHECK(vkCreateBuffer(dev,&(VkBufferCreateInfo){.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size=size,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT},NULL,&buf));
   VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev,buf,&mr);
   VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd,&mp);
   uint32_t mt=0;
   for(uint32_t i=0;i<mp.memoryTypeCount;i++)
      if((mr.memoryTypeBits&(1u<<i))&&(mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){mt=i;break;}
   VkDeviceMemory mem;
   CHECK(vkAllocateMemory(dev,&(VkMemoryAllocateInfo){.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize=mr.size,.memoryTypeIndex=mt},NULL,&mem));
   CHECK(vkBindBufferMemory(dev,buf,mem,0));

   VkDescriptorSetLayoutBinding b={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
   VkDescriptorSetLayout dsl;
   CHECK(vkCreateDescriptorSetLayout(dev,&(VkDescriptorSetLayoutCreateInfo){
      .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&b},NULL,&dsl));
   VkPushConstantRange pcr={.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT,.offset=0,.size=8};
   VkPipelineLayout pl;
   CHECK(vkCreatePipelineLayout(dev,&(VkPipelineLayoutCreateInfo){
      .sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&dsl,
      .pushConstantRangeCount=1,.pPushConstantRanges=&pcr},NULL,&pl));

   size_t l; uint32_t *code=load_spv(heavy_spv,&l);
   VkShaderModule sm;
   CHECK(vkCreateShaderModule(dev,&(VkShaderModuleCreateInfo){
      .sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=l,.pCode=code},NULL,&sm));
   VkPipeline pipe;
   CHECK(vkCreateComputePipelines(dev,VK_NULL_HANDLE,1,&(VkComputePipelineCreateInfo){
      .sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=sm,.pName="main"},.layout=pl},NULL,&pipe));

   VkDescriptorPoolSize ps={.type=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1};
   VkDescriptorPool dp;
   CHECK(vkCreateDescriptorPool(dev,&(VkDescriptorPoolCreateInfo){
      .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&ps},NULL,&dp));
   VkDescriptorSet ds;
   CHECK(vkAllocateDescriptorSets(dev,&(VkDescriptorSetAllocateInfo){
      .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=dp,
      .descriptorSetCount=1,.pSetLayouts=&dsl},&ds));
   vkUpdateDescriptorSets(dev,1,&(VkWriteDescriptorSet){
      .sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=ds,.dstBinding=0,.descriptorCount=1,
      .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .pBufferInfo=&(VkDescriptorBufferInfo){.buffer=buf,.offset=0,.range=VK_WHOLE_SIZE}},0,NULL);

   CHECK(vkCreateQueryPool(dev,&(VkQueryPoolCreateInfo){
      .sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_TIMESTAMP,
      .queryCount=4},NULL,&qp));
   CHECK(vkCreateCommandPool(dev,&(VkCommandPoolCreateInfo){
      .sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=0},NULL,&cp));
   CHECK(vkAllocateCommandBuffers(dev,&(VkCommandBufferAllocateInfo){
      .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cp,
      .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1},&cb));

   struct { uint32_t loops, stride; } pc;

   /* Bracket HEAVY with queries 0..1 and TRIVIAL with 2..3. Both are separate
    * control streams because a timestamp write ends the compute stream. */
   printf("%-34s %12s %12s\n", "case", "heavy ms", "trivial ms");
   for (int trivial_after_heavy = 0; trivial_after_heavy <= 1; trivial_after_heavy++) {
      CHECK(vkResetCommandPool(dev,cp,0));
      CHECK(vkBeginCommandBuffer(cb,&(VkCommandBufferBeginInfo){
         .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}));
      vkCmdResetQueryPool(cb,qp,0,4);
      vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipe);
      vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pl,0,1,&ds,0,NULL);

      vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,qp,0);
      if (trivial_after_heavy) {
         pc.loops = 200000; pc.stride = 64;
         vkCmdPushConstants(cb,pl,VK_SHADER_STAGE_COMPUTE_BIT,0,8,&pc);
         vkCmdDispatch(cb,1,1,1);          /* HEAVY: 32 threads, long loop */
      }
      vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,qp,1);

      vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,qp,2);
      pc.loops = 1; pc.stride = 64;
      vkCmdPushConstants(cb,pl,VK_SHADER_STAGE_COMPUTE_BIT,0,8,&pc);
      vkCmdDispatch(cb,1,1,1);             /* TRIVIAL: 32 threads, no work */
      vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,qp,3);
      CHECK(vkEndCommandBuffer(cb));

      CHECK(vkQueueSubmit(q,1,&(VkSubmitInfo){.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount=1,.pCommandBuffers=&cb},VK_NULL_HANDLE));
      CHECK(vkQueueWaitIdle(q));

      uint64_t ts[4]={0,0,0,0};
      CHECK(vkGetQueryPoolResults(dev,qp,0,4,sizeof(ts),ts,sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
      printf("%-34s %12.3f %12.3f\n",
             trivial_after_heavy ? "trivial AFTER a heavy dispatch"
                                 : "trivial alone",
             (double)(ts[1]-ts[0])*period/1e6, (double)(ts[3]-ts[2])*period/1e6);
   }
   printf("\nIf 'trivial' costs the same in both rows, the interval measures\n"
          "execution. If it inflates when it follows heavy work, it includes\n"
          "waiting and per-command timing cannot be read as per-shader cost.\n");
   return 0;
}
