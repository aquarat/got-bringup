/*
 * Does hk's vkd3d drirc profile actually reach the driver?
 *
 * src/asahi/vulkan/00-hk-defaults.conf turns on four options when the engine
 * name matches DXVK or vkd3d, three of which decide the reported D3D feature
 * level:
 *
 *   hk_enable_vertex_pipeline_stores_atomics   "Needed for FL11_1"
 *   hk_fake_minmax                             "We need this for FL12_0"
 *   hk_image_view_min_lod                      "We need this for FL12_0"
 *   hk_disable_border_emulation                (the "Honeykrisp is slow" one)
 *
 * Ghost of Tsushima logs "Max supported feature level: 11.0", which is what
 * vkd3d reports when vertexPipelineStoresAndAtomics is missing. So either the
 * match is not firing or the config file is not being found. This creates two
 * instances -- one anonymous, one claiming to be vkd3d -- and prints the one
 * feature that gates FL11_1. If both say false, the config never loaded.
 *
 * Build: cc -O2 -o drirctest drirctest.c -lvulkan
 */
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>

static int probe(const char *engine)
{
   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "test",
      .pEngineName = engine,
      .engineVersion = 1,
      .apiVersion = VK_API_VERSION_1_3,
   };
   VkInstance inst;
   if (vkCreateInstance(&(VkInstanceCreateInfo){
          .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
          .pApplicationInfo = &app}, NULL, &inst) != VK_SUCCESS)
      return -1;

   uint32_t n = 0;
   vkEnumeratePhysicalDevices(inst, &n, NULL);
   VkPhysicalDevice *pds = calloc(n, sizeof(*pds));
   vkEnumeratePhysicalDevices(inst, &n, pds);

   for (uint32_t i = 0; i < n; i++) {
      VkPhysicalDeviceProperties p;
      vkGetPhysicalDeviceProperties(pds[i], &p);
      if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
         continue;
      VkPhysicalDeviceFeatures f;
      vkGetPhysicalDeviceFeatures(pds[i], &f);
      printf("  engine=%-8s %-28s vertexPipelineStoresAndAtomics = %s\n",
             engine ? engine : "(none)", p.deviceName,
             f.vertexPipelineStoresAndAtomics ? "TRUE" : "false");
   }
   free(pds);
   vkDestroyInstance(inst, NULL);
   return 0;
}

int main(void)
{
   printf("DRIRC_CONFIGDIR = %s\n\n", getenv("DRIRC_CONFIGDIR") ?: "(unset)");
   probe(NULL);
   probe("vkd3d");
   probe("DXVK");
   return 0;
}
