#include "dlss.cpp"

#include <cassert>
#include <vector>

struct Presentation {
    uint32_t image;
    VkSemaphore semaphore;
};

static std::vector<Presentation> presentations;
static VkResult intermediate_result = VK_SUCCESS;

VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue, const VkPresentInfoKHR* info) {
    assert(info->swapchainCount == 1 && info->pSwapchains[0] == dlss_fg.swapchain);
    assert(info->waitSemaphoreCount == 1);
    presentations.push_back({info->pImageIndices[0], info->pWaitSemaphores[0]});
    return presentations.size() == 1 ? intermediate_result : VK_SUCCESS;
}

int main() {
    dlss_fg.ready = true;
    dlss_fg.swapchain = reinterpret_cast<VkSwapchainKHR>(1);
    const auto queue = reinterpret_cast<VkQueue>(1);
    const auto real_ready = reinterpret_cast<VkSemaphore>(2);
    const auto intermediate_ready = reinterpret_cast<VkSemaphore>(3);
    for (auto result : {VK_SUCCESS, VK_SUBOPTIMAL_KHR, VK_ERROR_OUT_OF_DATE_KHR,
                        VK_ERROR_SURFACE_LOST_KHR}) {
        intermediate_result = result;
        presentations.clear();
        assert(mk8_dlss_fg_present(queue, dlss_fg.swapchain, 1, 2, real_ready, intermediate_ready) == result);
        assert(presentations[0].image == 2 && presentations[0].semaphore == intermediate_ready);
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
            assert(presentations.size() == 2);
            assert(presentations[1].image == 1 && presentations[1].semaphore == real_ready);
        } else {
            assert(presentations.size() == 1);
        }
    }
}
