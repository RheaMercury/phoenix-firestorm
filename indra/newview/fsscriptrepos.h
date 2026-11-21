/**
 * @file fsscriptrepos.h
 * @brief Script repositories: repository references (GitHub, GitLab, Gitea/Forgejo, Bitbucket, zip links), zip
 *        reading, file rules and sync decisions.
 *
 * Pure logic (llcommon and minizip-ng only) so it can be unit tested.
 */

#ifndef FS_SCRIPTREPOS_H
#define FS_SCRIPTREPOS_H

#include "lldate.h"
#include "llsd.h"
#include "lluuid.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace FSScriptRepos
{
    constexpr size_t MAX_ZIP_BYTES = 52428800;  // 50 MB per archive, downloaded or unpacked
    constexpr size_t MAX_ZIP_ENTRIES = 10000;
    constexpr size_t MAX_TEXT_BYTES = 65536;    // script and notecard text
    constexpr size_t MAX_NAME_BYTES = 63;       // inventory names (DB_INV_ITEM_NAME_STR_LEN)
    const std::string DEFAULT_IGNORE("*.test.*");

    // Where a repository lives. ZIP: a plain https link to a zip file, no API and no commits.
    enum class Provider { GITHUB, GITLAB, GITEA, BITBUCKET, ZIP };
    std::string providerName(Provider provider);    // stable, for the state file
    bool providerFromName(const std::string& name, Provider& out);

    struct RepoRef
    {
        Provider provider{ Provider::GITHUB };
        std::string host;   // lower case, may end in ":port"; github.com, gitlab.com, codeberg.org, bitbucket.org, ...
        std::string owner;  // GitLab: the whole namespace ("group/subgroup"); ZIP: the host
        std::string repo;   // ZIP: the file name without .zip
        std::string ref;    // branch or tag, empty = default branch (not used for ZIP)
        std::string url;    // ZIP only: the https link
        // "owner/repo" on github.com; "host/owner/repo" elsewhere; the link (without its query) for ZIP
        std::string display() const;
        // Same repository, same branch: same key
        std::string key() const;
    };

    // Accepted forms, a non-empty branch winning over the link's ref:
    //   owner/repo                                    GitHub
    //   https://github.com/owner/repo[.git][/tree/<ref>]
    //   https://gitlab.com/group[/subgroup...]/repo[.git][/-/tree/<ref>]
    //   https://codeberg.org/owner/repo[.git][/src/branch/<ref> | /src/tag/<ref>]   (Gitea, Forgejo)
    //   https://bitbucket.org/workspace/repo[.git][/src/<ref>]
    //   https://host/.../file.zip                     any host: a zip link
    // The kind is told by the host (gitlab.com, codeberg.org, gitea.com, bitbucket.org, github.com; a host with
    // "gitlab", "gitea" or "forgejo" in its name), or by a link ending in .zip. A self-hosted instance with any other
    // name needs forced (the window's kind choice); without it the text is refused.
    bool parseRepoRef(const std::string& text, const std::string& branch, RepoRef& out);
    bool parseRepoRef(const std::string& text, const std::string& branch, const Provider* forced, RepoRef& out);
    // A host name as typed in a link: letters, digits, '-', '.', and an optional ":port"
    bool isValidHost(const std::string& host);
    // A name git accepts for a branch or tag (no "..", spaces, control characters, ~^:?*[\ or "@{").
    bool isValidRef(const std::string& ref);

    // The name the inventory keeps for an item or folder called name (as LLInventoryObject::correctInventoryName:
    // characters outside printable ASCII and '|' become spaces, trimmed, at most MAX_NAME_BYTES).
    std::string inWorldName(const std::string& name);

    enum class Kind { LSL, LUA, NOTECARD, IMAGE, SKIPPED };

    struct Entry
    {
        std::string path;           // repo-relative, '/' separated
        std::string folder;         // in-world folder path under the repository folder, '/' separated, "" at the root
        std::string name;           // in-world name
        std::string data;
        std::string md5;
        Kind kind{ Kind::SKIPPED };
        std::string skip_reason;
    };

    std::vector<std::string> splitIgnore(const std::string& csv);
    // '*' matches any run of characters, '/' included.
    bool globMatch(const std::string& pattern, const std::string& path);
    bool isSafeZipPath(const std::string& path);
    Entry classify(const std::string& path, std::string data, const std::vector<std::string>& ignore, bool images);
    // Line endings (CRLF, CR, LF) and trailing newlines ignored.
    bool sameText(const std::string& a, const std::string& b);
    std::string md5Hex(const std::string& data);

    // Reads a zip held in memory. When every file sits in one top folder (as in the archives GitHub, GitLab, Gitea
    // and Bitbucket make) that folder is left out of the paths. Unsafe paths go to unsafe_paths, not files. False
    // (with error) on a bad zip or over MAX_ZIP_BYTES / MAX_ZIP_ENTRIES.
    bool readZip(const std::string& buffer, std::vector<std::pair<std::string, std::string>>& files,
        std::vector<std::string>& unsafe_paths, std::string& error);

    struct FileState
    {
        std::string md5;
        LLUUID item_id;
        LLUUID asset_id;    // the text asset the sync saved; a script in a prim holding it has exactly this text
        bool compiled{ false };        // the synced script compiled; required to update objects you don't own
        bool compile_checked{ false }; // compiled is known (state from before it was recorded: unknown)
    };

    struct RepoState
    {
        RepoRef ref;
        std::string commit;
        LLDate synced;
        std::map<std::string, FileState> files;   // by repo path
    };

    LLSD toLLSD(const std::vector<RepoState>& repos);
    std::vector<RepoState> fromLLSD(const LLSD& sd);

    enum class InvAction { CREATE, UPDATE, UNCHANGED, GONE, SKIP };
    // entry == nullptr: the file is no longer in the repo.
    InvAction decideInventory(const Entry* entry, const FileState* state, bool item_present);

    enum class RegionAction { CURRENT, WOULD_CHANGE, LANGUAGE_DIFFERS, AMBIGUOUS, NOT_IN_REPO };
    // entries: the repo's scripts. source is set for CURRENT and WOULD_CHANGE.
    RegionAction decideRegion(const std::string& script_name, const std::string& old_text, bool old_is_lua,
        const std::vector<const Entry*>& entries, const Entry*& source);
}

#endif // FS_SCRIPTREPOS_H
