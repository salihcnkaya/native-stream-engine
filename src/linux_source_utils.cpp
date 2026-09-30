#if defined(__linux__)

#include "linux_source_utils.h"

#include <pipewire/pipewire.h>
#include <glib.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QDataStream>
#include <QVariantMap>
#include <fstream>

namespace {

struct WindowRestoreInfo {
    QString appId;
    QString title;
};

QDataStream& operator>>(
    QDataStream& in,
    WindowRestoreInfo& info
)
{
    return in >> info.appId >> info.title;
}

QDataStream& operator<<(
    QDataStream& out,
    const WindowRestoreInfo& info
)
{
    return out << info.appId << info.title;
}

void addUniqueAlias(
    std::vector<std::string>& aliases,
    const std::string& value
)
{
    if (value.empty()) {
        return;
    }

    const auto existing =
        std::find(
            aliases.begin(),
            aliases.end(),
            value
        );

    if (existing == aliases.end()) {
        aliases.push_back(value);
    }
}

std::string stripDesktopSuffix(
    const std::string& value
)
{
    static constexpr const char* suffix =
        ".desktop";

    if (
        value.size() >= 8 &&
        value.compare(
            value.size() - 8,
            8,
            suffix
        ) == 0
    ) {
        return value.substr(
            0,
            value.size() - 8
        );
    }

    return value;
}

std::string getExecBasename(
    const char* execValue
)
{
    if (
        !execValue ||
        !*execValue
    ) {
        return {};
    }

    gint argc = 0;
    gchar** argv = nullptr;
    GError* error = nullptr;

    if (
        !g_shell_parse_argv(
            execValue,
            &argc,
            &argv,
            &error
        )
    ) {
        if (error) {
            g_error_free(error);
        }

        return {};
    }

    std::string result;

    if (
        argc > 0 &&
        argv &&
        argv[0] &&
        *argv[0]
    ) {
        gchar* basename =
            g_path_get_basename(
                argv[0]
            );

        if (basename) {
            result = basename;
            g_free(basename);
        }
    }

    g_strfreev(argv);

    return result;
}

bool loadDesktopIdentity(
    const std::string& desktopPath,
    LinuxApplicationIdentity& identity
)
{
    GKeyFile* keyFile =
        g_key_file_new();

    if (!keyFile) {
        return false;
    }

    GError* error = nullptr;

    const gboolean loaded =
        g_key_file_load_from_file(
            keyFile,
            desktopPath.c_str(),
            G_KEY_FILE_NONE,
            &error
        );

    if (!loaded) {
        if (error) {
            g_error_free(error);
        }

        g_key_file_unref(keyFile);
        return false;
    }

    if (
        !g_key_file_has_group(
            keyFile,
            G_KEY_FILE_DESKTOP_GROUP
        )
    ) {
        g_key_file_unref(keyFile);
        return false;
    }

    identity.desktopPath =
        desktopPath;

    gchar* name =
        g_key_file_get_locale_string(
            keyFile,
            G_KEY_FILE_DESKTOP_GROUP,
            G_KEY_FILE_DESKTOP_KEY_NAME,
            nullptr,
            nullptr
        );

    if (name) {
        identity.displayName = name;
        g_free(name);
    }

    gchar* startupWMClass =
        g_key_file_get_string(
            keyFile,
            G_KEY_FILE_DESKTOP_GROUP,
            G_KEY_FILE_DESKTOP_KEY_STARTUP_WM_CLASS,
            nullptr
        );

    if (startupWMClass) {
        identity.startupWMClass =
            startupWMClass;

        g_free(startupWMClass);
    }

    gchar* exec =
        g_key_file_get_string(
            keyFile,
            G_KEY_FILE_DESKTOP_GROUP,
            G_KEY_FILE_DESKTOP_KEY_EXEC,
            nullptr
        );

    if (exec) {
        identity.execBasename =
            getExecBasename(exec);

        g_free(exec);
    }

    g_key_file_unref(keyFile);

    return true;
}

bool tryDesktopFile(
    const std::string& dataDirectory,
    const std::string& desktopId,
    LinuxApplicationIdentity& identity
)
{
    if (
        dataDirectory.empty() ||
        desktopId.empty()
    ) {
        return false;
    }

    gchar* path =
        g_build_filename(
            dataDirectory.c_str(),
            "applications",
            (
                desktopId +
                ".desktop"
            ).c_str(),
            nullptr
        );

    if (!path) {
        return false;
    }

    const std::string desktopPath =
        path;

    g_free(path);

    if (
        !g_file_test(
            desktopPath.c_str(),
            G_FILE_TEST_IS_REGULAR
        )
    ) {
        return false;
    }

    return loadDesktopIdentity(
        desktopPath,
        identity
    );
}

struct PipeWireEnumerationState;

struct BoundAudioNode {
    pw_node* node = nullptr;
    spa_hook listener{};
    PipeWireEnumerationState* state = nullptr;
};

struct BoundPipeWireClient {
    pw_client* client = nullptr;
    spa_hook listener{};
    uint32_t id = 0;
    PipeWireEnumerationState* state = nullptr;
};

struct PipeWireClientIdentity {
    std::string applicationName;
    std::string binary;
    uint32_t pid = 0;
};

struct PipeWireEnumerationState {
    pw_main_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    pw_registry* registry = nullptr;

    spa_hook registryListener{};
    spa_hook coreListener{};

    int syncSequence = 0;
    int syncPhase = 0;

    std::vector<
        std::unique_ptr<BoundAudioNode>
    > boundNodes;

    std::vector<
        std::unique_ptr<BoundPipeWireClient>
    > boundClients;

    std::unordered_map<
        uint32_t,
        PipeWireClientIdentity
    > clientIdentities;

    std::vector<LinuxAudioApplication> applications;
};

const char* dictValue(
    const spa_dict* props,
    const char* key
)
{
    if (!props || !key) {
        return nullptr;
    }

    return spa_dict_lookup(
        props,
        key
    );
}

uint32_t parsePid(
    const char* value
)
{
    if (!value || !*value) {
        return 0;
    }

    char* end = nullptr;

    const unsigned long parsed =
        std::strtoul(
            value,
            &end,
            10
        );

    if (
        end == value ||
        *end != '\0'
    ) {
        return 0;
    }

    return static_cast<uint32_t>(
        parsed
    );
}

void addApplication(
    PipeWireEnumerationState* state,
    const spa_dict* props
)
{
    if (!state || !props) {
        return;
    }

    const char* mediaClass =
        dictValue(
            props,
            PW_KEY_MEDIA_CLASS
        );

    if (
        !mediaClass ||
        std::strcmp(
            mediaClass,
            "Stream/Output/Audio"
        ) != 0
    ) {
        return;
    }

    const char* applicationName =
        dictValue(
            props,
            PW_KEY_APP_NAME
        );

    const char* binary =
        dictValue(
            props,
            PW_KEY_APP_PROCESS_BINARY
        );

    const char* pidString =
        dictValue(
            props,
            PW_KEY_APP_PROCESS_ID
        );

    const char* nodeName =
        dictValue(
            props,
            PW_KEY_NODE_NAME
        );

    const char* mediaRole =
        dictValue(
            props,
            PW_KEY_MEDIA_ROLE
        );

    const char* clientIdString =
        dictValue(
            props,
            PW_KEY_CLIENT_ID
        );

    const uint32_t clientId =
        parsePid(clientIdString);

    std::string resolvedBinary =
        binary && *binary
            ? binary
            : "";

    uint32_t resolvedPid =
        parsePid(pidString);

    std::string resolvedApplicationName =
        applicationName && *applicationName
            ? applicationName
            : "";
    
    if (
        resolvedBinary.empty() &&
        clientId != 0
    ) {
        const auto clientIt =
            state->clientIdentities.find(
                clientId
            );

        if (
            clientIt !=
            state->clientIdentities.end()
        ) {
            const auto& clientIdentity =
                clientIt->second;

            if (resolvedApplicationName.empty()) {
                resolvedApplicationName =
                    clientIdentity.applicationName;
            }

            resolvedBinary =
                clientIdentity.binary;

            if (resolvedPid == 0) {
                resolvedPid =
                    clientIdentity.pid;
            }
        }
    }

    if (resolvedBinary.empty()) {
        std::cerr
            << "[Native Stream Engine] "
            << "PipeWire audio node unresolved"
            << " application.name="
            << (
                applicationName && *applicationName
                    ? applicationName
                    : "<none>"
            )
            << " client.id="
            << clientId
            << " clientKnown="
            << (
                clientId != 0 &&
                state->clientIdentities.find(clientId) !=
                    state->clientIdentities.end()
                    ? "yes"
                    : "no"
            )
            << "\n";

        return;
    }

    LinuxAudioApplication app;

    app.name =
        !resolvedApplicationName.empty()
            ? resolvedApplicationName
            : resolvedBinary;

    app.binary =
        resolvedBinary;

    app.pid =
        resolvedPid;

    if (nodeName) {
        app.nodeName = nodeName;
    }

    if (mediaRole) {
        app.mediaRole = mediaRole;
    }

    const auto existing =
        std::find_if(
            state->applications.begin(),
            state->applications.end(),
            [&](const LinuxAudioApplication& candidate) {
                return
                    candidate.binary == app.binary &&
                    candidate.pid == app.pid;
            }
        );

    if (
        existing ==
        state->applications.end()
    ) {
        const char* debugAppName =
            spa_dict_lookup(props, "application.name");

        const char* debugAppId =
            spa_dict_lookup(props, "application.id");

        const char* debugBinary =
            spa_dict_lookup(props, "application.process.binary");

        const char* debugPid =
            spa_dict_lookup(props, "application.process.id");

        const char* debugNodeName =
            spa_dict_lookup(props, "node.name");

        const char* debugMediaName =
            spa_dict_lookup(props, "media.name");

        std::cerr
            << "[Native Stream Engine] PipeWire audio props begin\n";

        for (uint32_t i = 0; i < props->n_items; ++i) {
            const spa_dict_item& item = props->items[i];

            std::cerr
                << "[Native Stream Engine]   "
                << (item.key ? item.key : "<null>")
                << "="
                << (item.value ? item.value : "<null>")
                << "\n";
        }

        std::cerr
            << "[Native Stream Engine] PipeWire audio props end\n";

        std::cerr
            << "[Native Stream Engine] PipeWire audio identity"
            << " application.name=" << (debugAppName ? debugAppName : "<none>")
            << " application.id=" << (debugAppId ? debugAppId : "<none>")
            << " application.process.binary=" << (debugBinary ? debugBinary : "<none>")
            << " application.process.id=" << (debugPid ? debugPid : "<none>")
            << " node.name=" << (debugNodeName ? debugNodeName : "<none>")
            << " media.name=" << (debugMediaName ? debugMediaName : "<none>")
            << "\n";

        std::cerr
            << "[Native Stream Engine] "
            << "PipeWire audio application discovered"
            << " name="
            << app.name
            << " binary="
            << app.binary
            << " pid="
            << app.pid
            << " mediaRole="
            << (
                app.mediaRole.empty()
                    ? "<none>"
                    : app.mediaRole
            )
            << "\n";

        state->applications.push_back(
            std::move(app)
        );
    }
}

void nodeInfo(
    void* data,
    const pw_node_info* info
)
{
    auto* bound =
        static_cast<BoundAudioNode*>(
            data
        );

    if (
        !bound ||
        !bound->state ||
        !info ||
        !info->props
    ) {
        return;
    }

    addApplication(
        bound->state,
        info->props
    );
}

void clientInfo(
    void* data,
    const pw_client_info* info
)
{
    auto* bound =
        static_cast<BoundPipeWireClient*>(
            data
        );

    if (
        !bound ||
        !bound->state ||
        !info ||
        !info->props
    ) {
        return;
    }

    PipeWireClientIdentity identity;

    const char* applicationName =
        dictValue(
            info->props,
            PW_KEY_APP_NAME
        );

    const char* binary =
        dictValue(
            info->props,
            PW_KEY_APP_PROCESS_BINARY
        );

    const char* pidString =
        dictValue(
            info->props,
            PW_KEY_APP_PROCESS_ID
        );

    if (applicationName && *applicationName) {
        identity.applicationName =
            applicationName;
    }

    if (binary && *binary) {
        identity.binary =
            binary;
    }

    identity.pid =
        parsePid(pidString);

    bound->state->clientIdentities[
        bound->id
    ] = std::move(identity);
}

const pw_client_events clientEvents = {
    PW_VERSION_CLIENT_EVENTS,
    .info = clientInfo,
};

const pw_node_events nodeEvents = {
    PW_VERSION_NODE_EVENTS,
    .info = nodeInfo,
};

void registryGlobal(
    void* data,
    uint32_t id,
    uint32_t,
    const char* type,
    uint32_t version,
    const spa_dict* props
)
{
    auto* state =
        static_cast<PipeWireEnumerationState*>(
            data
        );

    if (!state || !type) {
        return;
    }

    if (
        std::strcmp(
            type,
            PW_TYPE_INTERFACE_Client
        ) == 0
    ) {
        auto bound =
            std::make_unique<BoundPipeWireClient>();

        bound->id = id;
        bound->state = state;

        bound->client =
            static_cast<pw_client*>(
                pw_registry_bind(
                    state->registry,
                    id,
                    PW_TYPE_INTERFACE_Client,
                    std::min(
                        version,
                        static_cast<uint32_t>(
                            PW_VERSION_CLIENT
                        )
                    ),
                    0
                )
            );

        if (!bound->client) {
            std::cerr
                << "[Native Stream Engine] "
                << "failed to bind PipeWire client id="
                << id
                << "\n";

            return;
        }

        pw_client_add_listener(
            bound->client,
            &bound->listener,
            &clientEvents,
            bound.get()
        );

        state->boundClients.push_back(
            std::move(bound)
        );

        return;
    }

    if (
        std::strcmp(
            type,
            PW_TYPE_INTERFACE_Node
        ) != 0
    ) {
        return;
    }

    const char* mediaClass =
        dictValue(
            props,
            PW_KEY_MEDIA_CLASS
        );

    if (
        !mediaClass ||
        std::strcmp(
            mediaClass,
            "Stream/Output/Audio"
        ) != 0
    ) {
        return;
    }

    auto bound =
        std::make_unique<BoundAudioNode>();

    bound->state = state;

    bound->node =
        static_cast<pw_node*>(
            pw_registry_bind(
                state->registry,
                id,
                PW_TYPE_INTERFACE_Node,
                std::min(
                    version,
                    static_cast<uint32_t>(
                        PW_VERSION_NODE
                    )
                ),
                0
            )
        );

    if (!bound->node) {
        std::cerr
            << "[Native Stream Engine] "
            << "failed to bind PipeWire audio node id="
            << id
            << "\n";

        return;
    }

    pw_node_add_listener(
        bound->node,
        &bound->listener,
        &nodeEvents,
        bound.get()
    );

    state->boundNodes.push_back(
        std::move(bound)
    );
}

void registryGlobalRemove(
    void*,
    uint32_t
)
{
}

const pw_registry_events registryEvents = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = registryGlobal,
    .global_remove = registryGlobalRemove,
};

void coreDone(
    void* data,
    uint32_t id,
    int sequence
)
{
    auto* state =
        static_cast<PipeWireEnumerationState*>(
            data
        );

    if (
        !state ||
        !state->loop ||
        id != PW_ID_CORE ||
        sequence != state->syncSequence
    ) {
        return;
    }

    if (state->syncPhase == 0) {
        state->syncPhase = 1;

        state->syncSequence =
            pw_core_sync(
                state->core,
                PW_ID_CORE,
                0
            );

        return;
    }

    pw_main_loop_quit(
        state->loop
    );
}

void coreError(
    void* data,
    uint32_t,
    int,
    int result,
    const char* message
)
{
    auto* state =
        static_cast<PipeWireEnumerationState*>(
            data
        );

    std::cerr
        << "[Native Stream Engine] "
        << "PipeWire enumeration error: "
        << result
        << " "
        << (
            message
                ? message
                : ""
        )
        << "\n";

    if (
        state &&
        state->loop
    ) {
        pw_main_loop_quit(
            state->loop
        );
    }
}

const pw_core_events coreEvents = {
    PW_VERSION_CORE_EVENTS,
    .done = coreDone,
    .error = coreError,
};

} // namespace


LinuxApplicationIdentity
resolveLinuxApplicationIdentity(
    const std::string& portalIdentity
)
{
    LinuxApplicationIdentity identity;

    identity.portalId =
        stripDesktopSuffix(
            portalIdentity
        );

    identity.desktopId =
        identity.portalId;

    addUniqueAlias(
        identity.aliases,
        identity.portalId
    );

    if (identity.portalId.empty()) {
        return identity;
    }

    bool resolved = false;

    const char* userDataDir =
        g_get_user_data_dir();

    if (
        userDataDir &&
        *userDataDir
    ) {
        resolved =
            tryDesktopFile(
                userDataDir,
                identity.desktopId,
                identity
            );
    }

    if (!resolved) {
        const gchar* const* systemDataDirs =
            g_get_system_data_dirs();

        if (systemDataDirs) {
            for (
                size_t i = 0;
                systemDataDirs[i] != nullptr;
                ++i
            ) {
                if (
                    tryDesktopFile(
                        systemDataDirs[i],
                        identity.desktopId,
                        identity
                    )
                ) {
                    resolved = true;
                    break;
                }
            }
        }
    }

    addUniqueAlias(
        identity.aliases,
        identity.desktopId
    );

    addUniqueAlias(
        identity.aliases,
        identity.displayName
    );

    addUniqueAlias(
        identity.aliases,
        identity.startupWMClass
    );

    addUniqueAlias(
        identity.aliases,
        identity.execBasename
    );

    std::cerr
        << "[Native Stream Engine] Linux app identity"
        << " portalId="
        << identity.portalId
        << " desktopId="
        << identity.desktopId
        << " resolved="
        << (resolved ? "yes" : "no")
        << " name="
        << (
            identity.displayName.empty()
                ? "<none>"
                : identity.displayName
        )
        << " startupWMClass="
        << (
            identity.startupWMClass.empty()
                ? "<none>"
                : identity.startupWMClass
        )
        << " exec="
        << (
            identity.execBasename.empty()
                ? "<none>"
                : identity.execBasename
        )
        << " path="
        << (
            identity.desktopPath.empty()
                ? "<none>"
                : identity.desktopPath
        )
        << " aliases=[";

    for (
        size_t i = 0;
        i < identity.aliases.size();
        ++i
    ) {
        if (i > 0) {
            std::cerr << ", ";
        }

        std::cerr
            << identity.aliases[i];
    }

    std::cerr
        << "]\n";

    return identity;
}

std::vector<LinuxAudioApplication>
listLinuxAudioApplications()
{
    PipeWireEnumerationState state;

    pw_init(
        nullptr,
        nullptr
    );

    state.loop =
        pw_main_loop_new(
            nullptr
        );

    if (!state.loop) {
        std::cerr
            << "[Native Stream Engine] "
            << "failed to create PipeWire main loop\n";

        pw_deinit();
        return {};
    }

    state.context =
        pw_context_new(
            pw_main_loop_get_loop(
                state.loop
            ),
            nullptr,
            0
        );

    if (!state.context) {
        std::cerr
            << "[Native Stream Engine] "
            << "failed to create PipeWire context\n";

        pw_main_loop_destroy(
            state.loop
        );

        pw_deinit();
        return {};
    }

    state.core =
        pw_context_connect(
            state.context,
            nullptr,
            0
        );

    if (!state.core) {
        std::cerr
            << "[Native Stream Engine] "
            << "failed to connect to PipeWire\n";

        pw_context_destroy(
            state.context
        );

        pw_main_loop_destroy(
            state.loop
        );

        pw_deinit();
        return {};
    }

    pw_core_add_listener(
        state.core,
        &state.coreListener,
        &coreEvents,
        &state
    );

    state.registry =
        pw_core_get_registry(
            state.core,
            PW_VERSION_REGISTRY,
            0
        );

    if (!state.registry) {
        std::cerr
            << "[Native Stream Engine] "
            << "failed to get PipeWire registry\n";

        pw_core_disconnect(
            state.core
        );

        pw_context_destroy(
            state.context
        );

        pw_main_loop_destroy(
            state.loop
        );

        pw_deinit();
        return {};
    }

    pw_registry_add_listener(
        state.registry,
        &state.registryListener,
        &registryEvents,
        &state
    );

    state.syncPhase = 0;

    state.syncSequence =
        pw_core_sync(
            state.core,
            PW_ID_CORE,
            0
        );

    pw_main_loop_run(
        state.loop
    );

    for (
        auto& bound :
        state.boundNodes
    ) {
        if (!bound) {
            continue;
        }

        spa_hook_remove(
            &bound->listener
        );

        if (bound->node) {
            pw_proxy_destroy(
                reinterpret_cast<pw_proxy*>(
                    bound->node
                )
            );

            bound->node = nullptr;
        }
    }

    state.boundNodes.clear();

    for (
        auto& bound :
        state.boundClients
    ) {
        if (!bound) {
            continue;
        }

        spa_hook_remove(
            &bound->listener
        );

        if (bound->client) {
            pw_proxy_destroy(
                reinterpret_cast<pw_proxy*>(
                    bound->client
                )
            );

            bound->client = nullptr;
        }
    }

    state.boundClients.clear();

    spa_hook_remove(
        &state.registryListener
    );

    spa_hook_remove(
        &state.coreListener
    );

    pw_proxy_destroy(
        reinterpret_cast<pw_proxy*>(
            state.registry
        )
    );

    pw_core_disconnect(
        state.core
    );

    pw_context_destroy(
        state.context
    );

    pw_main_loop_destroy(
        state.loop
    );

    pw_deinit();

    std::sort(
        state.applications.begin(),
        state.applications.end(),
        [](
            const LinuxAudioApplication& a,
            const LinuxAudioApplication& b
        ) {
            if (a.name != b.name) {
                return a.name < b.name;
            }

            if (a.binary != b.binary) {
                return a.binary < b.binary;
            }

            return a.pid < b.pid;
        }
    );

    return state.applications;
}

static std::string resolveLinuxWindowsExecutableFromPid(
    uint32_t pid
)
{
    if (pid == 0) {
        return {};
    }

    const std::string cmdlinePath =
        "/proc/" + std::to_string(pid) + "/cmdline";

    std::ifstream stream(
        cmdlinePath,
        std::ios::in | std::ios::binary
    );

    if (!stream) {
        return {};
    }

    std::string firstArgument;

    if (!std::getline(stream, firstArgument, '\0')) {
        return {};
    }

    if (firstArgument.empty()) {
        return {};
    }

    const size_t separator =
        firstArgument.find_last_of("/\\");

    const std::string basename =
        separator == std::string::npos
            ? firstArgument
            : firstArgument.substr(separator + 1);

    if (basename.empty()) {
        return {};
    }

    return basename;
}

static std::string normalizeLinuxIdentityValue(
    const std::string& value
)
{
    std::string normalized = value;

    std::transform(
        normalized.begin(),
        normalized.end(),
        normalized.begin(),
        [](unsigned char ch) {
            return static_cast<char>(
                std::tolower(ch)
            );
        }
    );

    while (
        !normalized.empty() &&
        std::isspace(
            static_cast<unsigned char>(
                normalized.front()
            )
        )
    ) {
        normalized.erase(
            normalized.begin()
        );
    }

    while (
        !normalized.empty() &&
        std::isspace(
            static_cast<unsigned char>(
                normalized.back()
            )
        )
    ) {
        normalized.pop_back();
    }

    return normalized;
}

const LinuxAudioApplication*
findExactLinuxAudioApplicationMatch(
    const LinuxApplicationIdentity& identity,
    const std::vector<LinuxAudioApplication>& audioApps
)
{
    const std::string normalizedDisplayName =
        normalizeLinuxIdentityValue(
            identity.displayName
        );

    const std::string normalizedExec =
        normalizeLinuxIdentityValue(
            identity.execBasename
        );

    const std::string normalizedStartupClass =
        normalizeLinuxIdentityValue(
            identity.startupWMClass
        );

    for (const auto& audioApp : audioApps) {
        const std::string normalizedAudioName =
            normalizeLinuxIdentityValue(
                audioApp.name
            );

        const std::string normalizedBinary =
            normalizeLinuxIdentityValue(
                audioApp.binary
            );

        const std::string normalizedNodeName =
            normalizeLinuxIdentityValue(
                audioApp.nodeName
            );

        if (
            !normalizedDisplayName.empty() &&
            normalizedAudioName ==
                normalizedDisplayName
        ) {
            return &audioApp;
        }

        if (
            !normalizedExec.empty() &&
            normalizedBinary ==
                normalizedExec
        ) {
            return &audioApp;
        }

        if (
            !normalizedStartupClass.empty() &&
            (
                normalizedAudioName ==
                    normalizedStartupClass ||
                normalizedBinary ==
                    normalizedStartupClass ||
                normalizedNodeName ==
                    normalizedStartupClass
            )
        ) {
            return &audioApp;
        }

        for (const auto& alias : identity.aliases) {
            const std::string normalizedAlias =
                normalizeLinuxIdentityValue(
                    alias
                );

            if (normalizedAlias.empty()) {
                continue;
            }

            if (
                normalizedAlias ==
                    normalizedAudioName ||
                normalizedAlias ==
                    normalizedBinary ||
                normalizedAlias ==
                    normalizedNodeName
            ) {
                return &audioApp;
            }
        }
    }

    return nullptr;
}

const LinuxAudioApplication*
findUniqueLinuxGameAudioApplication(
    const std::vector<LinuxAudioApplication>& audioApps
)
{
    const LinuxAudioApplication* match = nullptr;

    for (const auto& audioApp : audioApps) {
        if (
            normalizeLinuxIdentityValue(
                audioApp.mediaRole
            ) != "game"
        ) {
            continue;
        }

        if (match) {
            return nullptr;
        }

        match = &audioApp;
    }

    return match;
}

const LinuxAudioApplication*
findUniqueLinuxWineAudioApplication(
    const std::vector<LinuxAudioApplication>& audioApps
)
{
    const LinuxAudioApplication* match = nullptr;

    for (const auto& audioApp : audioApps) {
        if (
            normalizeLinuxIdentityValue(
                audioApp.binary
            ) != "wine64-preloader"
        ) {
            continue;
        }

        if (audioApp.name.empty()) {
            continue;
        }

        if (match) {
            return nullptr;
        }

        match = &audioApp;
    }

    return match;
}

LinuxPortalWindowIdentity
resolveLinuxPortalWindowIdentityFromRestoreToken(
    const std::string& restoreToken
)
{
    LinuxPortalWindowIdentity result;

    if (restoreToken.empty()) {
        return result;
    }

    qRegisterMetaType<
        QList<WindowRestoreInfo>
    >(
        "QList<WindowRestoreInfo>"
    );

    QDBusInterface permissionStore(
        "org.freedesktop.impl.portal.PermissionStore",
        "/org/freedesktop/impl/portal/PermissionStore",
        "org.freedesktop.impl.portal.PermissionStore",
        QDBusConnection::sessionBus()
    );

    if (!permissionStore.isValid()) {
        std::cerr
            << "[Native Stream Engine] "
            << "PermissionStore interface unavailable\n";

        return result;
    }

    const QDBusMessage reply =
        permissionStore.call(
            "Lookup",
            QStringLiteral("screencast"),
            QString::fromStdString(restoreToken)
        );

    if (
        reply.type() == QDBusMessage::ErrorMessage ||
        reply.arguments().size() < 2
    ) {
        std::cerr
            << "[Native Stream Engine] "
            << "PermissionStore Lookup failed for restore token\n";

        return result;
    }

    const QVariant restoreVariant =
        reply.arguments().at(1);

    if (
        restoreVariant.metaType().id() !=
        QMetaType::fromType<QDBusVariant>().id()
    ) {
        return result;
    }

    const QVariant innerVariant =
        restoreVariant.value<QDBusVariant>().variant();

    if (
        innerVariant.metaType().id() !=
        QMetaType::fromType<QDBusArgument>().id()
    ) {
        return result;
    }

    const QDBusArgument dbusArgument =
        innerVariant.value<QDBusArgument>();

    QString session;
    quint32 version = 0;
    QDBusVariant payloadVariant;

    dbusArgument.beginStructure();
    dbusArgument >> session;
    dbusArgument >> version;
    dbusArgument >> payloadVariant;
    dbusArgument.endStructure();

    if (
        session != QStringLiteral("KDE") ||
        version != 1
    ) {
        std::cerr
            << "[Native Stream Engine] "
            << "unsupported restore payload"
            << " session="
            << session.toStdString()
            << " version="
            << version
            << "\n";

        return result;
    }

    const QByteArray payloadBytes =
        payloadVariant.variant().toByteArray();

    if (payloadBytes.isEmpty()) {
        return result;
    }

    QVariantMap payload;

    QDataStream stream(
        payloadBytes
    );

    stream >> payload;

    const auto windows =
        payload.value(
            QStringLiteral("windows")
        ).value<
            QList<WindowRestoreInfo>
        >();

    if (windows.size() != 1) {
        std::cerr
            << "[Native Stream Engine] "
            << "portal restore window count="
            << windows.size()
            << "\n";

        return result;
    }

    result.appId =
        windows.front().appId.toStdString();

    result.title =
        windows.front().title.toStdString();

    std::cerr
        << "[Native Stream Engine] "
        << "portal restore identity"
        << " appId="
        << result.appId
        << " title="
        << result.title
        << "\n";

    return result;
}

LinuxPortalWindowMatch resolveLinuxPortalWindowMatch(
    const LinuxPortalWindowIdentity& portalIdentity
)
{
    if (
        portalIdentity.appId.empty() ||
        portalIdentity.title.empty()
    ) {
        return {};
    }

    QDBusInterface runner(
        "org.kde.KWin",
        "/WindowsRunner",
        "org.kde.krunner1",
        QDBusConnection::sessionBus()
    );

    if (!runner.isValid()) {
        std::cerr
            << "[Native Stream Engine] "
            << "KWin WindowsRunner unavailable\n";

        return {};
    }

    const QDBusMessage matchReply =
        runner.call(
            "Match",
            QString::fromStdString(
                portalIdentity.title
            )
        );

    if (
        matchReply.type() ==
            QDBusMessage::ErrorMessage ||
        matchReply.arguments().empty()
    ) {
        std::cerr
            << "[Native Stream Engine] "
            << "KWin WindowsRunner Match failed\n";

        return {};
    }

    const QVariant matchesVariant =
        matchReply.arguments().front();

    if (
        matchesVariant.metaType().id() !=
        QMetaType::fromType<QDBusArgument>().id()
    ) {
        return {};
    }

    const std::string normalizedPortalAppId =
        normalizeLinuxIdentityValue(
            portalIdentity.appId
        );

    const std::string normalizedPortalTitle =
        normalizeLinuxIdentityValue(
            portalIdentity.title
        );

    const QDBusArgument matchesArgument =
        matchesVariant.value<QDBusArgument>();

    LinuxPortalWindowMatch resolved;

    matchesArgument.beginArray();

    while (!matchesArgument.atEnd()) {
        QString matchId;
        QString text;
        QString subtext;
        quint32 type = 0;
        double relevance = 0.0;
        QVariantMap properties;

        matchesArgument.beginStructure();

        matchesArgument
            >> matchId
            >> text
            >> subtext
            >> type
            >> relevance
            >> properties;

        matchesArgument.endStructure();

        const QString prefix =
            QStringLiteral("0_");

        if (!matchId.startsWith(prefix)) {
            continue;
        }

        const QString uuid =
            matchId.mid(
                prefix.size()
            );

        if (
            uuid.isEmpty() ||
            !uuid.startsWith('{') ||
            !uuid.endsWith('}')
        ) {
            continue;
        }

        QDBusInterface kwin(
            "org.kde.KWin",
            "/KWin",
            "org.kde.KWin",
            QDBusConnection::sessionBus()
        );

        if (!kwin.isValid()) {
            continue;
        }

        const QDBusMessage infoReply =
            kwin.call(
                "getWindowInfo",
                uuid
            );

        if (
            infoReply.type() ==
                QDBusMessage::ErrorMessage ||
            infoReply.arguments().empty()
        ) {
            continue;
        }

        QVariantMap windowInfo;

        const QVariant infoVariant =
            infoReply.arguments().front();

        if (
            infoVariant.canConvert<
                QVariantMap
            >()
        ) {
            windowInfo =
                infoVariant.toMap();
        } else if (
            infoVariant.metaType().id() ==
            QMetaType::fromType<
                QDBusArgument
            >().id()
        ) {
            const QDBusArgument infoArgument =
                infoVariant.value<
                    QDBusArgument
                >();

            windowInfo =
                qdbus_cast<QVariantMap>(
                    infoArgument
                );
        }

        if (windowInfo.empty()) {
            continue;
        }

        const std::string caption =
            windowInfo.value(
                QStringLiteral("caption")
            ).toString().toStdString();

        const std::string resourceClass =
            windowInfo.value(
                QStringLiteral("resourceClass")
            ).toString().toStdString();

        const bool captionMatches =
            normalizeLinuxIdentityValue(
                caption
            ) ==
            normalizedPortalTitle;

        const bool appIdMatches =
            normalizeLinuxIdentityValue(
                resourceClass
            ) ==
            normalizedPortalAppId;

        if (
            !captionMatches ||
            !appIdMatches
        ) {
            continue;
        }

        const uint32_t pid =
            windowInfo.value(
                QStringLiteral("pid")
            ).toUInt();

        if (pid == 0) {
            continue;
        }

        const std::string resolvedUuid =
            uuid.toStdString();

        if (
            !resolved.uuid.empty() &&
            resolved.uuid != resolvedUuid
        ) {
            std::cerr
                << "[Native Stream Engine] "
                << "KWin portal window match ambiguous"
                << " appId="
                << portalIdentity.appId
                << " title="
                << portalIdentity.title
                << "\n";

            matchesArgument.endArray();
            return {};
        }

        resolved.uuid = resolvedUuid;
        resolved.pid = pid;
    }

    matchesArgument.endArray();

    if (resolved.valid()) {
        std::cerr
            << "[Native Stream Engine] "
            << "KWin portal window resolved"
            << " appId="
            << portalIdentity.appId
            << " title="
            << portalIdentity.title
            << " uuid="
            << resolved.uuid
            << " pid="
            << resolved.pid
            << "\n";
    }

    return resolved;
}

LinuxPortalWindowState queryLinuxPortalWindowState(
    const std::string& uuid
)
{
    if (uuid.empty()) {
        return LinuxPortalWindowState::Unknown;
    }

    QDBusInterface kwin(
        "org.kde.KWin",
        "/KWin",
        "org.kde.KWin",
        QDBusConnection::sessionBus()
    );

    if (!kwin.isValid()) {
        std::cerr
            << "[Native Stream Engine] "
            << "KWin interface unavailable while checking window"
            << " uuid="
            << uuid
            << "\n";

        return LinuxPortalWindowState::Unknown;
    }

    const QDBusMessage reply =
        kwin.call(
            "getWindowInfo",
            QString::fromStdString(uuid)
        );

    if (reply.type() == QDBusMessage::ErrorMessage) {
        std::cerr
            << "[Native Stream Engine] "
            << "KWin getWindowInfo failed while checking window"
            << " uuid="
            << uuid
            << "\n";

        return LinuxPortalWindowState::Unknown;
    }

    if (reply.arguments().empty()) {
        return LinuxPortalWindowState::Unknown;
    }

    QVariantMap windowInfo;

    const QVariant infoVariant =
        reply.arguments().front();

    if (
        infoVariant.canConvert<
            QVariantMap
        >()
    ) {
        windowInfo =
            infoVariant.toMap();
    } else if (
        infoVariant.metaType().id() ==
        QMetaType::fromType<
            QDBusArgument
        >().id()
    ) {
        const QDBusArgument infoArgument =
            infoVariant.value<
                QDBusArgument
            >();

        windowInfo =
            qdbus_cast<QVariantMap>(
                infoArgument
            );
    } else {
        return LinuxPortalWindowState::Unknown;
    }

    if (windowInfo.empty()) {
        return LinuxPortalWindowState::Closed;
    }

    if (
        windowInfo.value(
            QStringLiteral("deleted")
        ).toBool()
    ) {
        return LinuxPortalWindowState::Closed;
    }

    return LinuxPortalWindowState::Alive;
}

uint32_t resolveLinuxPortalWindowPid(
    const LinuxPortalWindowIdentity& portalIdentity
)
{
    return resolveLinuxPortalWindowMatch(
        portalIdentity
    ).pid;
}

std::string resolveLinuxExecutableTargetFromPid(
    uint32_t pid
)
{
    if (pid == 0) {
        return {};
    }

    const std::string executable =
        resolveLinuxWindowsExecutableFromPid(
            pid
        );

    if (executable.empty()) {
        std::cerr
            << "[Native Stream Engine] "
            << "portal window executable unresolved"
            << " pid="
            << pid
            << "\n";

        return {};
    }

    std::cerr
        << "[Native Stream Engine] "
        << "portal window executable resolved"
        << " pid="
        << pid
        << " executable="
        << executable
        << "\n";

    return executable;
}

std::string resolveLinuxPortalAudioTarget(
    const LinuxPortalWindowIdentity& portalIdentity,
    const std::vector<LinuxAudioApplication>& audioApps
)
{
    if (!portalIdentity.valid()) {
        return {};
    }

    const std::string normalizedPortalAppId =
        normalizeLinuxIdentityValue(
            portalIdentity.appId
        );

    const std::string normalizedPortalTitle =
        normalizeLinuxIdentityValue(
            portalIdentity.title
        );

    const auto stripExeSuffix =
        [](
            const std::string& value
        ) {
            std::string normalized =
                normalizeLinuxIdentityValue(
                    value
                );

            constexpr const char* exeSuffix =
                ".exe";

            if (
                normalized.size() >= 4 &&
                normalized.compare(
                    normalized.size() - 4,
                    4,
                    exeSuffix
                ) == 0
            ) {
                normalized.resize(
                    normalized.size() - 4
                );
            }

            return normalized;
        };

    std::string resolvedTarget;

    for (const auto& audioApp : audioApps) {
        const std::string normalizedAudioName =
            normalizeLinuxIdentityValue(
                audioApp.name
            );

        const std::string normalizedBinary =
            normalizeLinuxIdentityValue(
                audioApp.binary
            );

        const std::string normalizedNodeName =
            normalizeLinuxIdentityValue(
                audioApp.nodeName
            );

        const std::string audioNameWithoutExe =
            stripExeSuffix(
                audioApp.name
            );

        const std::string nodeNameWithoutExe =
            stripExeSuffix(
                audioApp.nodeName
            );

        const bool appIdMatch =
            !normalizedPortalAppId.empty() &&
            (
                normalizedPortalAppId ==
                    normalizedBinary ||
                normalizedPortalAppId ==
                    normalizedAudioName ||
                normalizedPortalAppId ==
                    normalizedNodeName
            );

        const bool titleMatch =
            !normalizedPortalTitle.empty() &&
            (
                normalizedPortalTitle ==
                    normalizedAudioName ||
                normalizedPortalTitle ==
                    normalizedNodeName ||
                normalizedPortalTitle ==
                    audioNameWithoutExe ||
                normalizedPortalTitle ==
                    nodeNameWithoutExe
            );

        if (!appIdMatch && !titleMatch) {
            continue;
        }

        std::string candidateTarget;

        if (
            normalizedBinary ==
            "wine64-preloader"
        ) {
            candidateTarget =
                audioApp.name;
        } else if (!audioApp.binary.empty()) {
            candidateTarget =
                audioApp.binary;
        } else {
            candidateTarget =
                audioApp.name;
        }

        if (candidateTarget.empty()) {
            continue;
        }

        if (resolvedTarget.empty()) {
            resolvedTarget =
                candidateTarget;

            continue;
        }

        if (
            normalizeLinuxIdentityValue(
                resolvedTarget
            ) !=
            normalizeLinuxIdentityValue(
                candidateTarget
            )
        ) {
            std::cerr
                << "[Native Stream Engine] "
                << "portal audio target ambiguous"
                << " existing="
                << resolvedTarget
                << " candidate="
                << candidateTarget
                << "\n";

            return {};
        }
    }

    return resolvedTarget;
}

#endif