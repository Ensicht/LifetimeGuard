// 进程内状态和固定容量缓冲。保持声明顺序及 ABI，不在此分配运行期扫描任务。
dstorage_guard::LooseTextureCleanup g_loose_cleanup;
std::atomic<void *> g_loose_cleanup_host{};

constexpr std::uint32_t k_version = 9;
constexpr std::uint32_t k_dstoragecore_timestamp = 0xE682ADC0;
constexpr std::uint32_t k_dstoragecore_image_size = 0x00178000;
constexpr std::uintptr_t k_public_file_close_rva = 0x3D1F0;
constexpr std::uintptr_t k_public_enqueue_request_rva = 0x3D460;
constexpr std::uintptr_t k_request_ctor_rva = 0x4655C;
constexpr std::uintptr_t k_request_try_complete_rva = 0x466AC;
constexpr std::size_t k_file_capacity = 32768;
constexpr std::size_t k_file_probe_limit = 256;
constexpr std::size_t k_file_lock_count = 256;
constexpr std::size_t k_request_capacity = 65536;
constexpr std::size_t k_request_probe_limit = 256;
constexpr std::size_t k_event_capacity = 8;
constexpr std::size_t k_stack_capacity = 16;
constexpr std::size_t k_resource_event_capacity = 32;
constexpr std::size_t k_loading_event_capacity = 256;
constexpr std::size_t k_install_event_capacity = 64;
constexpr std::size_t k_install_code_bytes = 16;
constexpr unsigned k_report_history_count = 8;

constexpr std::uint32_t k_lifecycle_environment_start = 1U << 0U;
constexpr std::uint32_t k_lifecycle_environment_end = 1U << 1U;
constexpr std::uint32_t k_lifecycle_player_end = 1U << 2U;
constexpr std::uint32_t k_lifecycle_fast_travel_start = 1U << 3U;
constexpr std::uint32_t k_lifecycle_camera_fade_end = 1U << 4U;
constexpr std::uint32_t k_lifecycle_update_motion = 1U << 5U;
constexpr std::uint32_t k_lifecycle_game_flow_loading = 1U << 6U;
constexpr std::uint32_t k_lifecycle_scene_identity = 1U << 7U;
constexpr std::uint32_t k_lifecycle_offline_test = 1U << 31U;

constexpr int k_mh_status_not_attempted = 0x7FFFFFFF;

constexpr std::uint64_t k_empty_key = 0;
constexpr std::uint64_t k_tombstone_key = 1;

constexpr std::uint64_t k_count_mask = 0xFFFFFFFFULL;
constexpr std::uint64_t k_close_pending = 1ULL << 32U;
constexpr std::uint64_t k_close_claimed = 1ULL << 33U;

constexpr std::uint64_t k_request_status_mask = 0x3ULL;
constexpr std::uint64_t k_request_status_empty = 0x0ULL;
constexpr std::uint64_t k_request_status_live = 0x1ULL;
constexpr std::uint64_t k_request_status_busy = 0x2ULL;
constexpr std::uint64_t k_request_generation_step = 0x4ULL;

using DStorageGetFactoryFn = HRESULT(WINAPI *)(REFIID, void **);
using PublicFileCloseFn = void (*)(void *);
using PublicEnqueueRequestFn = void (*)(void *, const void *);
using RequestCtorFn = void *(*)(void *, const void *);
using RequestTryCompleteFn = void *(*)(void *, void *);
using ComRefFn = ULONG (*)(void *);
using ResourceTaskReadyFn = bool (*)(void *);
using GameFlowLoadingFn = bool (*)(REFrameworkVMContextHandle, REFrameworkManagedObjectHandle);
using NativeObjectGetterFn = void *(*)(REFrameworkVMContextHandle, void *);

struct alignas(64) FileState {
    std::atomic<std::uint64_t> key{};
    std::atomic<std::uint64_t> wrapper{};
    std::atomic<std::uint64_t> internal_file{};
    std::atomic<std::uint64_t> state{};
    std::atomic<std::uint64_t> event_sequence{};
};

struct alignas(64) RequestLease {
    std::atomic<std::uint64_t> state{};
    std::atomic<std::uint64_t> request_id{};
    std::atomic<std::uint64_t> request{};
    std::atomic<std::uint64_t> wrapper{};
    std::atomic<std::uint64_t> internal_file{};
    std::atomic<FileState *> file_state{};
};

struct DeferredCloseEvent {
    std::atomic<std::uint64_t> commit{};
    std::atomic<std::uint64_t> replay_commit{};
    std::atomic<std::uint32_t> replay_reported{};
    std::uint32_t reserved{};
    std::uint64_t qpc{};
    std::uint64_t replay_qpc{};
    std::uint64_t wrapper{};
    std::uint64_t internal_file{};
    std::uint32_t thread_id{};
    std::uint32_t replay_thread_id{};
    std::uint32_t outstanding{};
    std::uint32_t stack_count{};
    std::uint64_t stack[k_stack_capacity]{};
};

struct PendingEnqueue {
    void *wrapper{};
    void *internal_file{};
    FileState *file_state{};
    bool consumed{};
    PendingEnqueue *previous{};
};

struct ResourceRecoveryEvent {
    std::atomic<std::uint64_t> commit{};
    std::uint64_t qpc{};
    std::uint64_t task{};
    std::uint64_t resource{};
    std::uint64_t expected_bytes{};
    std::uint64_t completed_bytes{};
    std::uint32_t thread_id{};
    std::uint32_t decision{};
    std::uint64_t generation{};
    dstorage_guard::ResourceRecoverySnapshot snapshot{};
};

enum class LoadingEventKind : std::uint32_t {
    start = 1,
    end = 2,
    signal = 3,
};

enum class LoadingEventSource : std::uint32_t {
    environment_before = 1,
    environment_end = 2,
    player_end = 3,
    fast_travel_setup = 4,
    camera_fade_in = 5,
    offline_test = 6,
    stable_scene = 7,
};

enum class InstallComponent : std::uint32_t {
    lifetime = 1,
    resource = 2,
    texture_retry = 3,
};

enum class InstallStage : std::uint32_t {
    locate_ready = 1,
    locate_finalizer = 2,
    locate_texture_vtable = 3,
    validate_target = 4,
    mh_initialize = 5,
    mh_create = 6,
    mh_queue_enable = 7,
    mh_apply_queued = 8,
    mh_enable = 9,
    mh_disable_rollback = 10,
    mh_remove_rollback = 11,
    allocator_result = 12,
    validate_resource_worker = 13,
    locate_task_slot = 14,
    pointer_install = 15,
    pointer_restore = 16,
    pointer_rollback = 17,
    validate_task_submit = 18,
};

struct LoadingGateEvent {
    std::atomic<std::uint64_t> commit{};
    std::uint64_t qpc{};
    std::uint64_t generation{};
    std::uint64_t active_leases{};
    std::uint32_t thread_id{};
    std::uint32_t kind{};
    std::uint32_t source{};
    std::uint32_t reserved{};
};

struct InstallEvent {
    std::atomic<std::uint64_t> commit{};
    std::uint64_t qpc{};
    std::uint64_t target{};
    std::uint64_t detour{};
    std::uint64_t original{};
    std::uint64_t module_base{};
    std::uint64_t redirect{};
    std::uint32_t thread_id{};
    std::uint32_t component{};
    std::uint32_t stage{};
    std::uint32_t index{};
    std::int32_t status{};
    std::uint32_t win32_error{};
    std::uint32_t protection{};
    std::uint32_t code_size{};
    std::uint8_t code[k_install_code_bytes]{};
    MinHookAllocationDiagnostics allocation{};
};

struct GuardStats {
    std::uint32_t version{};
    std::uint32_t install_state{};
    std::uint64_t guarded_requests{};
    std::uint64_t completed_requests{};
    std::uint64_t active_leases{};
    std::uint64_t deferred_closes{};
    std::uint64_t replayed_closes{};
    std::uint64_t unpaired_requests{};
    std::uint64_t registry_collisions{};
};

struct LoadingLifecycleStats {
    std::uint32_t version{};
    std::uint32_t lifecycle_state{};
    std::uint32_t active{};
    std::uint32_t requires_fade_in{};
    std::uint32_t end_seen{};
    std::uint32_t saw_loading{};
    std::uint64_t generation{};
};

std::atomic<DStorageGetFactoryFn> g_original_get_factory{};
std::atomic<PublicFileCloseFn> g_original_file_close{};
std::atomic<PublicEnqueueRequestFn> g_original_enqueue{};
std::atomic<RequestCtorFn> g_original_request_ctor{};
std::atomic<RequestTryCompleteFn> g_original_try_complete{};
std::atomic<ResourceTaskReadyFn> g_original_resource_task_ready{};
std::atomic<bool> g_hooks_armed{};
std::atomic<bool> g_resource_hook_armed{};
std::atomic<std::uint32_t> g_install_state{};
std::atomic<std::uint32_t> g_resource_install_state{};
std::atomic<std::uint32_t> g_resource_hook_mode{};
std::atomic<std::uint32_t> g_initialize_state{};
std::atomic<std::uint32_t> g_status_ui_state{};
dstorage_guard::GuardStatusUi g_status_ui{};
std::atomic<std::uint32_t> g_iat_hook_state{};
std::atomic<std::uint32_t> g_lifecycle_hook_state{};
std::atomic<bool> g_offline_test_mode{};
std::atomic<int> g_lifetime_create_hook_status{k_mh_status_not_attempted};
std::atomic<int> g_lifetime_queue_hook_status{k_mh_status_not_attempted};
std::atomic<int> g_lifetime_apply_hook_status{k_mh_status_not_attempted};
std::atomic<int> g_resource_create_hook_status{k_mh_status_not_attempted};
std::atomic<int> g_resource_enable_hook_status{k_mh_status_not_attempted};
std::atomic<std::uint32_t> g_lifetime_create_hook_index{};
std::atomic<std::uint64_t> g_guarded_requests{};
std::atomic<std::uint64_t> g_completed_requests{};
std::atomic<std::uint64_t> g_active_leases{};
std::atomic<std::uint64_t> g_deferred_closes{};
std::atomic<std::uint64_t> g_replayed_closes{};
std::atomic<std::uint64_t> g_unpaired_requests{};
std::atomic<std::uint64_t> g_registry_collisions{};
std::atomic<std::uint64_t> g_event_next{};
std::atomic<std::uint64_t> g_event_written{};
std::atomic<std::uint64_t> g_resource_event_next{};
std::atomic<std::uint64_t> g_resource_event_written{};
std::atomic<std::uint64_t> g_resource_mismatches{};
std::atomic<std::uint64_t> g_resource_recoveries{};
std::atomic<std::uint64_t> g_loading_event_next{};
std::atomic<std::uint64_t> g_loading_event_written{};
std::atomic<std::uint64_t> g_install_event_next{};
std::atomic<std::uint64_t> g_install_event_written{};
std::atomic<std::uint32_t> g_report_thread_state{};
std::atomic<bool> g_protection_report_requested{};
std::uint32_t g_report_create_error{};
std::uint64_t g_session_utc{};
std::uint32_t g_report_rotation_failures{};
std::uint32_t g_report_rotation_last_error{};
FileState g_files[k_file_capacity]{};
RequestLease g_requests[k_request_capacity]{};
DeferredCloseEvent g_events[k_event_capacity]{};
ResourceRecoveryEvent g_resource_events[k_resource_event_capacity]{};
LoadingGateEvent g_loading_events[k_loading_event_capacity]{};
InstallEvent g_install_events[k_install_event_capacity]{};
SRWLOCK g_file_locks[k_file_lock_count]{};
thread_local PendingEnqueue *g_pending_enqueue{};
HANDLE g_report_event{};
HANDLE g_report_thread{};
HMODULE g_module{};
void *g_texture_vtable{};
dstorage_guard::LoadingGate g_loading_gate{};
const REFrameworkSDKData *g_reframework_sdk{};
GameFlowLoadingFn g_game_flow_get_loading{};
NativeObjectGetterFn g_scene_get_current{};
NativeObjectGetterFn g_scene_get_main_view{};
