#pragma once

#if defined(__linux__)

#include <cstdint>
#include <string>
#include <vector>

struct LinuxAudioApplication {
    std::string name;
    std::string binary;
    uint32_t pid = 0;
    std::string nodeName;
    std::string mediaRole;
};

struct LinuxApplicationIdentity {
    std::string portalId;
    std::string desktopId;
    std::string desktopPath;

    std::string displayName;
    std::string startupWMClass;
    std::string execBasename;

    std::vector<std::string> aliases;
};

struct LinuxPortalWindowIdentity {
    std::string appId;
    std::string title;

    bool valid() const
    {
        return
            !appId.empty() ||
            !title.empty();
    }
};

struct LinuxPortalWindowMatch {
    std::string uuid;
    uint32_t pid = 0;

    bool valid() const
    {
        return !uuid.empty() && pid != 0;
    }
};

enum class LinuxPortalWindowState {
    Unknown,
    Alive,
    Closed
};

std::vector<LinuxAudioApplication>
listLinuxAudioApplications();

LinuxApplicationIdentity
resolveLinuxApplicationIdentity(
    const std::string& portalIdentity
);

const LinuxAudioApplication*
findExactLinuxAudioApplicationMatch(
    const LinuxApplicationIdentity& identity,
    const std::vector<LinuxAudioApplication>& audioApps
);

const LinuxAudioApplication*
findUniqueLinuxGameAudioApplication(
    const std::vector<LinuxAudioApplication>& audioApps
);

const LinuxAudioApplication*
findUniqueLinuxWineAudioApplication(
    const std::vector<LinuxAudioApplication>& audioApps
);

LinuxPortalWindowIdentity
resolveLinuxPortalWindowIdentityFromRestoreToken(
    const std::string& restoreToken
);

LinuxPortalWindowMatch resolveLinuxPortalWindowMatch(
    const LinuxPortalWindowIdentity& portalIdentity
);

LinuxPortalWindowState queryLinuxPortalWindowState(
    const std::string& uuid
);

uint32_t resolveLinuxPortalWindowPid(
    const LinuxPortalWindowIdentity& portalIdentity
);

std::string resolveLinuxPortalAudioTarget(
    const LinuxPortalWindowIdentity& portalIdentity,
    const std::vector<LinuxAudioApplication>& audioApps
);

std::string resolveLinuxExecutableTargetFromPid(
    uint32_t pid
);

#endif