#include "libs/dialog.h"

#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "libs/errno.h"
#include "libs/libs.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <chrono>
#include <thread>
#include <algorithm>
#include <string>
#include <vector>

// NOLINTNEXTLINE(modernize-concat-nested-namespaces)
namespace Libs::Dialog {

namespace {

constexpr int DIALOG_STATUS_RUNNING  = 2;
constexpr int DIALOG_STATUS_FINISHED = 3;

struct DialogState {
	int      status     = 0;
	uint64_t generation = 0;
};

std::mutex  g_dialog_mutex;
DialogState g_error;
DialogState g_signin;
uint64_t    g_dialog_generation        = 0;
uint64_t    g_dialog_revision          = 0;
int32_t     g_error_code               = 0;
void (*g_dialog_visibility_callback)() = nullptr;

void SetDialogStatus(DialogState& dialog, int status, std::unique_lock<std::mutex>& lock) {
	const bool visibility_changed =
	    (dialog.status == DIALOG_STATUS_RUNNING) != (status == DIALOG_STATUS_RUNNING);
	dialog.status = status;
	if (visibility_changed) {
		g_dialog_revision++;
		if (status == DIALOG_STATUS_RUNNING) {
			dialog.generation = ++g_dialog_generation;
		}
	}
	const auto callback = visibility_changed ? g_dialog_visibility_callback : nullptr;
	lock.unlock();
	if (callback != nullptr) {
		callback();
	}
}

} // namespace

namespace SystemDialog {

bool GetHostSnapshot(HostSnapshot* snapshot) {
	std::scoped_lock lock(g_dialog_mutex);
	if (g_signin.status == DIALOG_STATUS_RUNNING) {
		*snapshot = {Kind::Signin, g_signin.generation, 0};
		return true;
	}
	if (g_error.status == DIALOG_STATUS_RUNNING) {
		*snapshot = {Kind::Error, g_error.generation, g_error_code};
		return true;
	}
	return false;
}

VisualState GetVisualState() noexcept {
	std::scoped_lock lock(g_dialog_mutex);
	const bool       signin = g_signin.status == DIALOG_STATUS_RUNNING;
	return {g_error.status == DIALOG_STATUS_RUNNING || signin, signin, g_dialog_revision};
}

void SetVisibilityCallback(void (*callback)()) {
	std::scoped_lock lock(g_dialog_mutex);
	g_dialog_visibility_callback = callback;
}

bool HostClose(uint64_t generation) {
	std::unique_lock lock(g_dialog_mutex);
	auto&            dialog = g_signin.status == DIALOG_STATUS_RUNNING ? g_signin : g_error;
	if (dialog.status != DIALOG_STATUS_RUNNING || generation != dialog.generation) {
		return false;
	}
	SetDialogStatus(dialog, DIALOG_STATUS_FINISHED, lock);
	return true;
}

} // namespace SystemDialog

namespace CommonDialog {

LIB_NAME("CommonDialog", "CommonDialog");

int KYTY_SYSV_ABI CommonDialogInitialize() {
	PRINT_NAME();

	return OK;
}

bool KYTY_SYSV_ABI CommonDialogIsUsed() {
	PRINT_NAME();

	return false;
}

} // namespace CommonDialog

namespace LoginDialog {

LIB_NAME("LoginDialog", "LoginDialog");

constexpr int LOGIN_STATUS_NONE        = 0;
constexpr int LOGIN_STATUS_INITIALIZED = 1;
constexpr int LOGIN_STATUS_RUNNING     = 2;
constexpr int LOGIN_STATUS_FINISHED    = 3;
constexpr int LOGIN_RESULT_OK          = 0;
constexpr int LOGIN_MODE_ALL_USERS     = 0;
constexpr int LOGIN_MODE_NOT_LOGGED_IN = 1;
constexpr int LOGIN_USER_INVALID       = -1;

constexpr int LOGIN_ERROR_NOT_INITIALIZED     = static_cast<int>(0x81340001u);
constexpr int LOGIN_ERROR_ALREADY_INITIALIZED = static_cast<int>(0x81340002u);
constexpr int LOGIN_ERROR_PARAM_INVALID       = static_cast<int>(0x81340003u);
constexpr int LOGIN_ERROR_INVALID_STATE       = static_cast<int>(0x81340005u);

struct LoginDialogResult {
	int32_t result;
	int32_t selected_user;
	int32_t reserved[2];
};

struct LoginDialogParam {
	int32_t size;
	int32_t mode;
	int32_t exclude_users_from_login_list[4];
	int32_t exclude_users_from_logout_list[4];
	int32_t initial_focus;
	int32_t reserved[5];
};

static_assert(sizeof(LoginDialogResult) == 16);
static_assert(sizeof(LoginDialogParam) == 64);

static int g_login_status        = LOGIN_STATUS_NONE;
static int g_login_selected_user = LOGIN_USER_INVALID;

int KYTY_SYSV_ABI LoginDialogInitialize() {
	PRINT_NAME();

	if (g_login_status != LOGIN_STATUS_NONE) {
		return LOGIN_ERROR_ALREADY_INITIALIZED;
	}

	g_login_status        = LOGIN_STATUS_INITIALIZED;
	g_login_selected_user = Config::GetUserId();

	return OK;
}

int KYTY_SYSV_ABI LoginDialogTerminate() {
	PRINT_NAME();

	if (g_login_status == LOGIN_STATUS_NONE) {
		return LOGIN_ERROR_NOT_INITIALIZED;
	}

	g_login_status        = LOGIN_STATUS_NONE;
	g_login_selected_user = LOGIN_USER_INVALID;

	return OK;
}

int KYTY_SYSV_ABI LoginDialogOpen(const void* param) {
	PRINT_NAME();

	if (g_login_status != LOGIN_STATUS_INITIALIZED && g_login_status != LOGIN_STATUS_FINISHED) {
		return LOGIN_ERROR_INVALID_STATE;
	}
	if (param == nullptr) {
		return LOGIN_ERROR_PARAM_INVALID;
	}

	const auto* p = static_cast<const LoginDialogParam*>(param);
	if (p->size != sizeof(LoginDialogParam) ||
	    (p->mode != LOGIN_MODE_ALL_USERS && p->mode != LOGIN_MODE_NOT_LOGGED_IN)) {
		return LOGIN_ERROR_PARAM_INVALID;
	}

	g_login_selected_user =
	    p->initial_focus != LOGIN_USER_INVALID ? p->initial_focus : Config::GetUserId();

	LOGF("\t size          = %d\n"
	     "\t mode          = %d\n"
	     "\t initial_focus = %d\n"
	     "\t selected_user = %d\n",
	     p->size, p->mode, p->initial_focus, g_login_selected_user);

	g_login_status = LOGIN_STATUS_RUNNING;

	return OK;
}

int KYTY_SYSV_ABI LoginDialogClose() {
	PRINT_NAME();

	if (g_login_status == LOGIN_STATUS_NONE) {
		return LOGIN_ERROR_NOT_INITIALIZED;
	}

	g_login_status = LOGIN_STATUS_FINISHED;

	return OK;
}

int KYTY_SYSV_ABI LoginDialogUpdateStatus() {
	PRINT_NAME();

	if (g_login_status == LOGIN_STATUS_RUNNING) {
		g_login_status = LOGIN_STATUS_FINISHED;
	}

	return g_login_status;
}

int KYTY_SYSV_ABI LoginDialogGetStatus() {
	PRINT_NAME();

	return g_login_status;
}

int KYTY_SYSV_ABI LoginDialogGetResult(void* result) {
	PRINT_NAME();

	if (g_login_status == LOGIN_STATUS_NONE) {
		return LOGIN_ERROR_NOT_INITIALIZED;
	}
	if (result == nullptr) {
		return LOGIN_ERROR_PARAM_INVALID;
	}

	auto* r          = static_cast<LoginDialogResult*>(result);
	r->result        = LOGIN_RESULT_OK;
	r->selected_user = g_login_selected_user;
	r->reserved[0]   = 0;
	r->reserved[1]   = 0;

	return OK;
}

void KYTY_SYSV_ABI LoginDialogParamInitialize(void* param) {
	PRINT_NAME();

	if (param == nullptr) {
		return;
	}

	auto* p = static_cast<LoginDialogParam*>(param);
	std::memset(p, 0, sizeof(LoginDialogParam));
	p->size = sizeof(LoginDialogParam);
	for (int i = 0; i < 4; i++) {
		p->exclude_users_from_login_list[i]  = LOGIN_USER_INVALID;
		p->exclude_users_from_logout_list[i] = LOGIN_USER_INVALID;
	}
	p->initial_focus = LOGIN_USER_INVALID;
}

} // namespace LoginDialog

namespace SigninDialog {

LIB_NAME("SigninDialog", "SigninDialog");

constexpr int SIGNIN_STATUS_NONE          = 0;
constexpr int SIGNIN_STATUS_INITIALIZED   = 1;
constexpr int SIGNIN_STATUS_RUNNING       = 2;
constexpr int SIGNIN_STATUS_FINISHED      = 3;
constexpr int SIGNIN_RESULT_USER_CANCELED = 1;

constexpr int SIGNIN_ERROR_NOT_INITIALIZED     = static_cast<int>(0x81350001u);
constexpr int SIGNIN_ERROR_ALREADY_INITIALIZED = static_cast<int>(0x81350002u);
constexpr int SIGNIN_ERROR_PARAM_INVALID       = static_cast<int>(0x81350003u);
constexpr int SIGNIN_ERROR_INVALID_STATE       = static_cast<int>(0x81350005u);
constexpr int SIGNIN_ERROR_INVALID_USER_ID     = static_cast<int>(0x81350007u);

struct SigninDialogResult {
	int32_t result;
	int32_t reserved[3];
};

struct SigninDialogParam {
	int32_t size;
	int32_t user_id;
	int32_t reserved[2];
};

static_assert(sizeof(SigninDialogResult) == 16);
static_assert(sizeof(SigninDialogParam) == 16);

int KYTY_SYSV_ABI SigninDialogInitialize() {
	PRINT_NAME();
	std::scoped_lock lock(g_dialog_mutex);
	if (g_signin.status != SIGNIN_STATUS_NONE) {
		return SIGNIN_ERROR_ALREADY_INITIALIZED;
	}
	g_signin.status = SIGNIN_STATUS_INITIALIZED;
	return OK;
}

int KYTY_SYSV_ABI SigninDialogTerminate() {
	PRINT_NAME();
	std::unique_lock lock(g_dialog_mutex);
	if (g_signin.status == SIGNIN_STATUS_NONE) {
		return SIGNIN_ERROR_NOT_INITIALIZED;
	}
	SetDialogStatus(g_signin, SIGNIN_STATUS_NONE, lock);
	return OK;
}

int KYTY_SYSV_ABI SigninDialogOpen(const void* param) {
	PRINT_NAME();
	std::unique_lock lock(g_dialog_mutex);
	if (g_signin.status == SIGNIN_STATUS_NONE) {
		return SIGNIN_ERROR_NOT_INITIALIZED;
	}
	if (g_signin.status != SIGNIN_STATUS_INITIALIZED && g_signin.status != SIGNIN_STATUS_FINISHED) {
		return SIGNIN_ERROR_INVALID_STATE;
	}
	if (param == nullptr) {
		return SIGNIN_ERROR_PARAM_INVALID;
	}
	const auto* p = static_cast<const SigninDialogParam*>(param);
	if (p->size != sizeof(SigninDialogParam) || p->reserved[0] != 0 || p->reserved[1] != 0) {
		return SIGNIN_ERROR_PARAM_INVALID;
	}
	if (p->user_id != Config::GetUserId()) {
		return SIGNIN_ERROR_INVALID_USER_ID;
	}
	SetDialogStatus(g_signin, SIGNIN_STATUS_RUNNING, lock);
	return OK;
}

int KYTY_SYSV_ABI SigninDialogClose() {
	PRINT_NAME();
	std::unique_lock lock(g_dialog_mutex);
	if (g_signin.status == SIGNIN_STATUS_NONE) {
		return SIGNIN_ERROR_NOT_INITIALIZED;
	}
	SetDialogStatus(g_signin, SIGNIN_STATUS_FINISHED, lock);
	return OK;
}

int KYTY_SYSV_ABI SigninDialogUpdateStatus() {
	PRINT_NAME();
	std::scoped_lock lock(g_dialog_mutex);
	return g_signin.status;
}

int KYTY_SYSV_ABI SigninDialogGetStatus() {
	PRINT_NAME();
	std::scoped_lock lock(g_dialog_mutex);
	return g_signin.status;
}

int KYTY_SYSV_ABI SigninDialogGetResult(void* result) {
	PRINT_NAME();
	std::scoped_lock lock(g_dialog_mutex);
	if (g_signin.status == SIGNIN_STATUS_NONE) {
		return SIGNIN_ERROR_NOT_INITIALIZED;
	}
	if (g_signin.status != SIGNIN_STATUS_FINISHED) {
		return SIGNIN_ERROR_INVALID_STATE;
	}
	if (result == nullptr) {
		return SIGNIN_ERROR_PARAM_INVALID;
	}
	auto* r        = static_cast<SigninDialogResult*>(result);
	r->result      = SIGNIN_RESULT_USER_CANCELED;
	r->reserved[0] = 0;
	r->reserved[1] = 0;
	r->reserved[2] = 0;
	return OK;
}

} // namespace SigninDialog

namespace SaveDataDialog {

LIB_NAME("SaveDataDialog", "SaveDataDialog");

constexpr int SAVE_STATUS_NONE        = 0;
constexpr int SAVE_STATUS_INITIALIZED = 1;
constexpr int SAVE_STATUS_RUNNING     = 2;
constexpr int SAVE_STATUS_FINISHED    = 3;
constexpr int SAVE_RESULT_OK          = 0;
constexpr int SAVE_BUTTON_ID_OK       = 1;

struct SaveDataDialogParam {
	uint8_t  base_param[48];
	int32_t  size;
	int32_t  mode;
	int32_t  disp_type;
	uint32_t pad0;
	void*    anim_param;
	void*    items;
	void*    user_msg_param;
	void*    sys_msg_param;
	void*    error_code_param;
	void*    prog_bar_param;
	void*    user_data;
	void*    option_param;
	void*    wizard_param;
	uint8_t  reserved[16];
};

struct SaveDataDirName {
	char data[32];
};

struct SaveDataDialogItems {
	int32_t                user_id;
	int32_t                pad0;
	const void*            title_id;
	const SaveDataDirName* dir_names;
	uint32_t               dir_names_num;
};

struct SaveDataDialogResult {
	int32_t  mode;
	int32_t  result;
	int32_t  button_id;
	uint32_t pad0;
	void*    dir_name;
	void*    param;
	void*    user_data;
	uint8_t  reserved[32];
};

static std::atomic<int> g_save_status = SAVE_STATUS_NONE;
static std::mutex g_save_mutex;
static bool g_save_running_polled = false;
static int g_save_mode = 0;
static int g_save_disp_type = 0;
static std::string g_save_new_directory;
static void* g_save_user_data = nullptr;
static char g_save_dir_name[sizeof(SaveDataDirName::data)] {};
static std::vector<std::string> g_save_directories;
static bool g_save_selection_active = false;
static bool g_save_cancelled = false;
static uint64_t g_save_generation = 0;
static uint64_t g_save_revision = 0;
static void (*g_save_visibility_callback)() = nullptr;

bool GetHostSnapshot(HostSnapshot* snapshot) {
    if (snapshot == nullptr) return false;
    std::scoped_lock lock(g_save_mutex);
    if (!g_save_selection_active || g_save_status != SAVE_STATUS_RUNNING) return false;
    *snapshot = {g_save_generation, g_save_directories, 0,
                 g_save_disp_type == 1, g_save_new_directory};
    return true;
}

VisualState GetVisualState() noexcept {
    std::scoped_lock lock(g_save_mutex);
    return {g_save_selection_active && g_save_status == SAVE_STATUS_RUNNING, g_save_revision};
}

void SetVisibilityCallback(void (*callback)()) {
    std::scoped_lock lock(g_save_mutex);
    g_save_visibility_callback = callback;
}

bool HostSelect(uint64_t generation, int index) {
    void (*callback)() = nullptr;
    {
        std::scoped_lock lock(g_save_mutex);
        if (!g_save_selection_active || g_save_status != SAVE_STATUS_RUNNING ||
            generation != g_save_generation || index < 0 ||
            static_cast<size_t>(index) >= g_save_directories.size()) return false;
        std::snprintf(g_save_dir_name, sizeof(g_save_dir_name), "%s",
                      g_save_directories[static_cast<size_t>(index)].c_str());
        g_save_cancelled = false;
        g_save_selection_active = false;
        g_save_status = SAVE_STATUS_FINISHED;
        ++g_save_revision;
        callback = g_save_visibility_callback;
    }
    if (callback) callback();
    return true;
}

bool HostCreateNew(uint64_t generation) {
    void (*callback)() = nullptr;
    {
        std::scoped_lock lock(g_save_mutex);
        if (!g_save_selection_active || g_save_status != SAVE_STATUS_RUNNING ||
            generation != g_save_generation || g_save_disp_type != 1 ||
            g_save_new_directory.empty()) return false;
        std::snprintf(g_save_dir_name, sizeof(g_save_dir_name), "%s",
                      g_save_new_directory.c_str());
        LOGF("\t [SAVE UI] Creating new slot: %s\n", g_save_dir_name);
        g_save_cancelled = false;
        g_save_selection_active = false;
        g_save_status = SAVE_STATUS_FINISHED;
        ++g_save_revision;
        callback = g_save_visibility_callback;
    }
    if (callback) callback();
    return true;
}

bool HostCancel(uint64_t generation) {
    void (*callback)() = nullptr;
    {
        std::scoped_lock lock(g_save_mutex);
        if (!g_save_selection_active || g_save_status != SAVE_STATUS_RUNNING ||
            generation != g_save_generation) return false;
        g_save_cancelled = true;
        g_save_dir_name[0] = '\0';
        g_save_selection_active = false;
        g_save_status = SAVE_STATUS_FINISHED;
        ++g_save_revision;
        callback = g_save_visibility_callback;
    }
    if (callback) callback();
    return true;
}

int KYTY_SYSV_ABI SaveDataDialogInitialize() {
    PRINT_NAME();
    std::scoped_lock lock(g_save_mutex);
    g_save_status = SAVE_STATUS_INITIALIZED;
    g_save_running_polled = false;
    g_save_mode = 0;
    g_save_disp_type = 0;
    g_save_new_directory.clear();
    g_save_user_data = nullptr;
    g_save_dir_name[0] = '\0';
    g_save_directories.clear();
    g_save_selection_active = false;
    g_save_cancelled = false;
    return OK;
}

int KYTY_SYSV_ABI SaveDataDialogGetStatus() {
	PRINT_NAME();

	return g_save_status;
}

int KYTY_SYSV_ABI SaveDataDialogUpdateStatus() {
    PRINT_NAME();
    std::scoped_lock lock(g_save_mutex);
    const int old_status = g_save_status;
    const bool was_polled = g_save_running_polled;
    if (g_save_status == SAVE_STATUS_RUNNING && !g_save_selection_active) {
        if (g_save_running_polled) g_save_status = SAVE_STATUS_FINISHED;
        g_save_running_polled = true;
    }
    LOGF("\t [SAVE DEBUG] UpdateStatus: %d -> %d, previously_polled=%d\n",
         old_status, g_save_status.load(), static_cast<int>(was_polled));
    return g_save_status;
}

int KYTY_SYSV_ABI SaveDataDialogGetResult(void* result) {
    PRINT_NAME();

    std::scoped_lock lock(g_save_mutex);

    LOGF("\t [SAVE DEBUG] GetResult: result_ptr=%p, status=%d\n",
     result, g_save_status.load());

    if (result != nullptr) {
        auto* r      = static_cast<SaveDataDialogResult*>(result);
        r->mode      = g_save_mode;
        r->result    = g_save_cancelled ? 1 : SAVE_RESULT_OK;
        r->button_id = g_save_cancelled ? 0 : SAVE_BUTTON_ID_OK;
        r->user_data = g_save_user_data;

        if (r->dir_name != nullptr && g_save_dir_name[0] != '\0') {
            std::snprintf(static_cast<SaveDataDirName*>(r->dir_name)->data,
                          sizeof(SaveDataDirName::data), "%s", g_save_dir_name);
        }

        // Verify the actual result buffer returned to the game.
        LOGF("\t [SAVE DEBUG] Result buffer: dir_ptr=%p, param_ptr=%p, user_data=%p\n",
             r->dir_name, r->param, r->user_data);

        if (r->dir_name != nullptr) {
            LOGF("\t [SAVE DEBUG] Result directory: %.32s\n",
                 static_cast<SaveDataDirName*>(r->dir_name)->data);


        if (r->param != nullptr) {
            const auto* bytes =
                static_cast<const unsigned char*>(r->param);

            LOGF("\t [SAVE DEBUG] Result param first 16 bytes:"
                 " %02x %02x %02x %02x %02x %02x %02x %02x"
                 " %02x %02x %02x %02x %02x %02x %02x %02x\n",
                 static_cast<unsigned>(bytes[0]),
                 static_cast<unsigned>(bytes[1]),
                 static_cast<unsigned>(bytes[2]),
                 static_cast<unsigned>(bytes[3]),
                 static_cast<unsigned>(bytes[4]),
                 static_cast<unsigned>(bytes[5]),
                 static_cast<unsigned>(bytes[6]),
                 static_cast<unsigned>(bytes[7]),
                 static_cast<unsigned>(bytes[8]),
                 static_cast<unsigned>(bytes[9]),
                 static_cast<unsigned>(bytes[10]),
                 static_cast<unsigned>(bytes[11]),
                 static_cast<unsigned>(bytes[12]),
                 static_cast<unsigned>(bytes[13]),
                 static_cast<unsigned>(bytes[14]),
                 static_cast<unsigned>(bytes[15]));
        }

        }

        LOGF("\t [SAVE DEBUG] GetResult: mode=%d, result=%d, button=%d, dir=%s\n",
             r->mode, r->result, r->button_id, g_save_dir_name);
    }

    return OK;
}

int KYTY_SYSV_ABI SaveDataDialogOpen(const void* param) {
    PRINT_NAME();
    const auto* p = static_cast<const SaveDataDialogParam*>(param);
    std::vector<std::string> directories;
    if (p != nullptr) {
        LOGF("\t size=%d mode=%d disp_type=%d items=%p\n",
             p->size, p->mode, p->disp_type, p->items);
        const auto* items = static_cast<const SaveDataDialogItems*>(p->items);
        if (items && items->dir_names) {
            // Bound the number of guest entries copied into host-owned storage.
            constexpr uint32_t MAX_DIRS = 1024;
            const uint32_t count = std::min(items->dir_names_num, MAX_DIRS);
            for (uint32_t i = 0; i < count; ++i) {
                const auto* raw = items->dir_names[i].data;
                const size_t len = strnlen(raw, sizeof(SaveDataDirName::data));
                if (len != 0) directories.emplace_back(raw, len);
                LOGF("\t save dir[%u] = %.*s\n", i, static_cast<int>(len), raw);
            }
        }
    }

    // Experimental new-slot candidate: use the first unused SAVELIST#### name.
    // Only existing names supplied by the game are considered; the host does
    // not create a directory here. The game's subsequent save must do that.
    std::string new_directory;
    if (p != nullptr && p->mode == 1 && p->disp_type == 1) {
        for (unsigned slot = 0; slot <= 9999; ++slot) {
            char candidate[sizeof(SaveDataDirName::data)] {};
            std::snprintf(candidate, sizeof(candidate), "SAVELIST%04u", slot);
            if (std::find(directories.begin(), directories.end(), candidate) == directories.end()) {
                new_directory = candidate;
                break;
            }
        }
    }

    // Preserve the working-build experiment exactly for mode 5.
    if (p != nullptr && p->mode == 5) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    void (*callback)() = nullptr;
    {
        std::scoped_lock lock(g_save_mutex);
        g_save_mode = p ? p->mode : 0;
        g_save_disp_type = p ? p->disp_type : 0;
        g_save_new_directory = std::move(new_directory);
        g_save_user_data = p ? p->user_data : nullptr;
        g_save_dir_name[0] = '\0';
        g_save_directories = std::move(directories);
        g_save_cancelled = false;
        g_save_running_polled = false;
        if (!g_save_directories.empty()) {
            std::snprintf(g_save_dir_name, sizeof(g_save_dir_name), "%s",
                          g_save_directories.front().c_str());
        }
        // Only list mode with selectable entries uses the interactive overlay.
        g_save_selection_active = (g_save_mode == 1 &&
            (!g_save_directories.empty() || !g_save_new_directory.empty()));
        g_save_status = SAVE_STATUS_RUNNING;
        if (g_save_selection_active) {
            ++g_save_generation;
            ++g_save_revision;
            callback = g_save_visibility_callback;
        }
    }
    if (callback) callback();
    return OK;
}

int KYTY_SYSV_ABI SaveDataDialogClose(const void* close_param) {
    PRINT_NAME();
    LOGF("\t close_param = %p\n", close_param);
    void (*callback)() = nullptr;
    {
        std::scoped_lock lock(g_save_mutex);
        if (g_save_selection_active) {
            g_save_selection_active = false;
            ++g_save_revision;
            callback = g_save_visibility_callback;
        }
        g_save_status = SAVE_STATUS_FINISHED;
        g_save_running_polled = false;
    }
    if (callback) callback();
    return OK;
}

int KYTY_SYSV_ABI SaveDataDialogIsReadyToDisplay() {
	PRINT_NAME();

	return 1;
}

int KYTY_SYSV_ABI SaveDataDialogTerminate() {
    PRINT_NAME();
    void (*callback)() = nullptr;
    {
        std::scoped_lock lock(g_save_mutex);
        if (g_save_selection_active) {
            ++g_save_revision;
            callback = g_save_visibility_callback;
        }
        g_save_status = SAVE_STATUS_NONE;
        g_save_running_polled = false;
        g_save_mode = 0;
    g_save_disp_type = 0;
    g_save_new_directory.clear();
        g_save_user_data = nullptr;
        g_save_dir_name[0] = '\0';
        g_save_directories.clear();
        g_save_selection_active = false;
        g_save_cancelled = false;
    }
    if (callback) callback();
    return OK;
}

int KYTY_SYSV_ABI SaveDataDialogProgressBarInc(int target, uint32_t delta) {
	PRINT_NAME();

	LOGF("\t target = %d\n"
	     "\t delta  = %u\n",
	     target, delta);

	return OK;
}

int KYTY_SYSV_ABI SaveDataDialogProgressBarSetValue(int target, uint32_t rate) {
	PRINT_NAME();

	LOGF("\t target = %d\n"
	     "\t rate   = %u\n",
	     target, rate);

	return OK;
}

} // namespace SaveDataDialog

namespace MsgDialog {

LIB_NAME("MsgDialog.native", "MsgDialog");

constexpr int STATUS_NONE        = 0;
constexpr int STATUS_INITIALIZED = 1;
constexpr int STATUS_FINISHED    = 3;
constexpr int RESULT_OK          = 0;
constexpr int BUTTON_ID_OK       = 1;

struct MsgDialogParam {
	uint8_t  base_param[48];
	uint64_t size;
	int32_t  mode;
	uint32_t pad0;
	void*    user_msg_param;
	void*    prog_bar_param;
	void*    sys_msg_param;
	int32_t  user_id;
	uint8_t  reserved[40];
	uint32_t pad1;
};

struct MsgDialogResult {
	int32_t mode;
	int32_t result;
	int32_t button_id;
	uint8_t reserved[32];
};

static int g_status = STATUS_NONE;
static int g_mode   = 0;

int KYTY_SYSV_ABI MsgDialogInitialize() {
	PRINT_NAME();

	g_status = STATUS_INITIALIZED;
	g_mode   = 0;

	return OK;
}

int KYTY_SYSV_ABI MsgDialogOpen(const void* param) {
	PRINT_NAME();

	const auto* p = static_cast<const MsgDialogParam*>(param);
	if (p != nullptr) {
		g_mode = p->mode;
		LOGF("\t size           = 0x%016" PRIx64 "\n"
		     "\t mode           = %d\n"
		     "\t user_msg_param = 0x%016" PRIx64 "\n"
		     "\t prog_bar_param = 0x%016" PRIx64 "\n"
		     "\t sys_msg_param  = 0x%016" PRIx64 "\n"
		     "\t user_id        = %d\n",
		     p->size, p->mode, reinterpret_cast<uint64_t>(p->user_msg_param),
		     reinterpret_cast<uint64_t>(p->prog_bar_param),
		     reinterpret_cast<uint64_t>(p->sys_msg_param), p->user_id);
	}

	g_status = STATUS_FINISHED;

	return OK;
}

int KYTY_SYSV_ABI MsgDialogUpdateStatus() {
	PRINT_NAME();

	return g_status;
}

int KYTY_SYSV_ABI MsgDialogGetStatus() {
	PRINT_NAME();

	return g_status;
}

int KYTY_SYSV_ABI MsgDialogGetResult(void* result) {
	PRINT_NAME();

	if (result != nullptr) {
		auto* r      = static_cast<MsgDialogResult*>(result);
		r->mode      = g_mode;
		r->result    = RESULT_OK;
		r->button_id = BUTTON_ID_OK;
	}

	return OK;
}

int KYTY_SYSV_ABI MsgDialogTerminate() {
	PRINT_NAME();

	g_status = STATUS_NONE;
	g_mode   = 0;

	return OK;
}

int KYTY_SYSV_ABI MsgDialogClose() {
	PRINT_NAME();

	g_status = STATUS_FINISHED;

	return OK;
}

int KYTY_SYSV_ABI MsgDialogProgressBarInc(int target, uint32_t delta) {
	PRINT_NAME();

	LOGF("\t target = %d\n"
	     "\t delta  = %u\n",
	     target, delta);

	return OK;
}

int KYTY_SYSV_ABI MsgDialogProgressBarSetValue(int target, uint32_t rate) {
	PRINT_NAME();

	LOGF("\t target = %d\n"
	     "\t rate   = %u\n",
	     target, rate);

	return OK;
}

int KYTY_SYSV_ABI MsgDialogProgressBarSetMsg(int target, const char* msg) {
	PRINT_NAME();

	LOGF("\t target = %d\n"
	     "\t msg    = 0x%016" PRIx64 "\n",
	     target, reinterpret_cast<uint64_t>(msg));

	return OK;
}

} // namespace MsgDialog

namespace ErrorDialog {

LIB_NAME("ErrorDialog", "ErrorDialog");

constexpr int STATUS_NONE        = 0;
constexpr int STATUS_INITIALIZED = 1;
constexpr int STATUS_RUNNING     = 2;
constexpr int STATUS_FINISHED    = 3;

constexpr int ERROR_NOT_INITIALIZED     = static_cast<int>(0x80ed0001);
constexpr int ERROR_ALREADY_INITIALIZED = static_cast<int>(0x80ed0002);
constexpr int ERROR_PARAM_INVALID       = static_cast<int>(0x80ed0003);
constexpr int ERROR_INVALID_STATE       = static_cast<int>(0x80ed0005);

struct ErrorDialogParam {
	int32_t size;
	int32_t error_code;
	int32_t user_id;
	int32_t reserved;
};

static_assert(sizeof(ErrorDialogParam) == 16);

int KYTY_SYSV_ABI ErrorDialogInitialize() {
	PRINT_NAME();

	std::scoped_lock lock(g_dialog_mutex);
	if (g_error.status != STATUS_NONE) {
		return ERROR_ALREADY_INITIALIZED;
	}

	g_error.status = STATUS_INITIALIZED;

	return OK;
}

int KYTY_SYSV_ABI ErrorDialogOpen(const void* param) {
	PRINT_NAME();

	std::unique_lock lock(g_dialog_mutex);
	if (g_error.status != STATUS_INITIALIZED && g_error.status != STATUS_FINISHED) {
		return ERROR_INVALID_STATE;
	}
	if (param == nullptr) {
		return ERROR_PARAM_INVALID;
	}

	const auto* p = static_cast<const ErrorDialogParam*>(param);
	if (p->size != sizeof(ErrorDialogParam)) {
		return ERROR_PARAM_INVALID;
	}

	LOGF("\t size       = %d\n"
	     "\t error_code = 0x%08" PRIx32 "\n"
	     "\t user_id    = %d\n",
	     p->size, static_cast<uint32_t>(p->error_code), p->user_id);

	g_error_code = p->error_code;
	SetDialogStatus(g_error, STATUS_RUNNING, lock);

	return OK;
}

int KYTY_SYSV_ABI ErrorDialogClose() {
	PRINT_NAME();

	std::unique_lock lock(g_dialog_mutex);
	if (g_error.status != STATUS_RUNNING) {
		return ERROR_INVALID_STATE;
	}

	SetDialogStatus(g_error, STATUS_FINISHED, lock);

	return OK;
}

int KYTY_SYSV_ABI ErrorDialogTerminate() {
	PRINT_NAME();

	std::unique_lock lock(g_dialog_mutex);
	if (g_error.status == STATUS_NONE) {
		return ERROR_NOT_INITIALIZED;
	}

	SetDialogStatus(g_error, STATUS_NONE, lock);

	return OK;
}

int KYTY_SYSV_ABI ErrorDialogUpdateStatus() {
	PRINT_NAME();

	std::scoped_lock lock(g_dialog_mutex);
	return g_error.status;
}

int KYTY_SYSV_ABI ErrorDialogGetStatus() {
	PRINT_NAME();

	std::scoped_lock lock(g_dialog_mutex);
	return g_error.status;
}

} // namespace ErrorDialog

} // namespace Libs::Dialog
