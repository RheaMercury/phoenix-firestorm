/**
 * @file fsscriptreposfetch.cpp
 * @brief Script repositories: GitHub requests and the GitHub token.
 */

#include "llviewerprecompiledheaders.h"

#include "fsscriptreposfetch.h"

#include "llcorehttputil.h"
#include "llhttpconstants.h"
#include "llsecapi.h"
#include "llsdjson.h"
#include "lluri.h"

namespace FSScriptRepos
{
namespace
{
    const std::string TOKEN_TYPE("github_token");
    const std::string TOKEN_ID("default");
    const std::string API_ROOT("https://api.github.com/");
    constexpr unsigned int TRANSFER_TIMEOUT = 300;
    constexpr int MAX_REDIRECTS = 3;
    // GitHub's repository size (KB, the packed git data with its history). The zipball is downloaded whole into
    // memory before its size can be checked, so a repository far over the zip limit is refused before that.
    constexpr S32 MAX_REPO_KB = 256 * 1024;

    std::string repoPath(const RepoRef& ref)
    {
        return API_ROOT + "repos/" + LLURI::escape(ref.owner) + "/" + LLURI::escape(ref.repo);
    }

    std::string apiPath(const RepoRef& ref, const std::string& what, const std::string& arg)
    {
        return repoPath(ref) + "/" + what + "/" + LLURI::escapePathAndData(arg);
    }

    std::string header(const LLSD& results, const std::string& name)
    {
        const LLSD& headers = results[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_HEADERS];
        return headers.has(name) ? headers[name].asString() : std::string();
    }

    std::string errorFor(S32 code, const LLSD& results)
    {
        if (code == 403 && header(results, "x-ratelimit-remaining") == "0")
        {
            return "GitHub request limit reached. Try again later or add a token.";
        }
        switch (code)
        {
        case 401:
        case 403:
        case 404:
            return "Not found, or the token cannot read it.";
        case 409:
            return "The repository is empty.";
        case 422:
            return "Branch or tag not found.";
        default:
            return llformat("GitHub request failed (HTTP %d).", code);
        }
    }

    struct Response
    {
        S32 code{ 0 };
        LLSD results;
        std::string body;
    };

    // The Authorization header only goes to api.github.com; redirects are never followed automatically so it
    // cannot reach another host.
    Response get(const std::string& url, const std::string& accept)
    {
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter(new LLCoreHttpUtil::HttpCoroutineAdapter(
            "ScriptReposFetch", LLCore::HttpRequest::DEFAULT_POLICY_ID));
        LLCore::HttpRequest::ptr_t request(new LLCore::HttpRequest);
        LLCore::HttpOptions::ptr_t options(new LLCore::HttpOptions);
        LLCore::HttpHeaders::ptr_t headers(new LLCore::HttpHeaders);
        options->setWantHeaders(true);
        options->setFollowRedirects(false);
        options->setTransferTimeout(TRANSFER_TIMEOUT);
        headers->append(HTTP_OUT_HEADER_ACCEPT, accept);

        if (url.compare(0, API_ROOT.size(), API_ROOT) == 0)
        {
            headers->append("X-GitHub-Api-Version", "2022-11-28");
            const std::string token = loadToken();
            if (!token.empty())
            {
                headers->append("Authorization", "Bearer " + token);
            }
        }

        LLSD result = adapter->getRawAndSuspend(request, url, options, headers);

        Response response;
        response.results = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
        LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(response.results);
        response.code = status ? HTTP_OK : (status.isHttpStatus() ? status.getType() : 0);
        const LLSD::Binary& raw = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
        response.body.assign(raw.begin(), raw.end());

        // The query of a redirect URL is a short-lived download credential: never logged
        LL_INFOS("ScriptRepos") << "GET " << url.substr(0, url.find('?')) << " -> " << response.code << LL_ENDL;
        return response;
    }

    // Follows GitHub's redirects by hand (a renamed repository answers 301, a zipball 302 to codeload.github.com).
    // Each hop goes through get(), which adds the token only for api.github.com.
    Response getFollowing(const std::string& url, const std::string& accept, const std::string& redirect_accept)
    {
        Response response = get(url, accept);
        for (int hops = 0; hops < MAX_REDIRECTS &&
             (response.code == 301 || response.code == 302 || response.code == 307 || response.code == 308); ++hops)
        {
            const std::string location = header(response.results, "location");
            if (location.compare(0, 8, "https://") != 0)
            {
                response.code = -1;
                return response;
            }
            response = get(location, location.compare(0, API_ROOT.size(), API_ROOT) == 0 ? accept : redirect_accept);
        }
        return response;
    }

    std::string failure(const Response& response)
    {
        if (response.code < 0)
        {
            return "GitHub sent an unexpected answer.";
        }
        return response.code ? errorFor(response.code, response.results) : "Could not reach GitHub.";
    }
}

std::string loadToken()
{
    if (!gSecAPIHandler)
    {
        return std::string();
    }
    LLSD data = gSecAPIHandler->getProtectedData(TOKEN_TYPE, TOKEN_ID);
    return data.isString() ? data.asString() : std::string();
}

void saveToken(const std::string& token)
{
    if (!gSecAPIHandler)
    {
        return;
    }
    if (token.empty())
    {
        forgetToken();
        return;
    }
    gSecAPIHandler->setProtectedData(TOKEN_TYPE, TOKEN_ID, LLSD(token));
    gSecAPIHandler->syncProtectedMap();
}

void forgetToken()
{
    if (!gSecAPIHandler)
    {
        return;
    }
    gSecAPIHandler->deleteProtectedData(TOKEN_TYPE, TOKEN_ID);
    gSecAPIHandler->syncProtectedMap();
}

FetchResult fetchCommit(const RepoRef& ref)
{
    FetchResult out;
    Response response = getFollowing(apiPath(ref, "commits", ref.ref.empty() ? "HEAD" : ref.ref),
        "application/vnd.github.sha", "application/vnd.github.sha");
    if (response.code != HTTP_OK)
    {
        out.error = failure(response);
        return out;
    }

    std::string commit = response.body;
    LLStringUtil::trim(commit);
    if (commit.size() != 40 || commit.find_first_not_of("0123456789abcdef") != std::string::npos)
    {
        out.error = "GitHub sent an unexpected answer.";
        return out;
    }
    out.ok = true;
    out.commit = commit;
    return out;
}

FetchResult fetchZipball(const RepoRef& ref, const std::string& commit)
{
    FetchResult out;
    out.commit = commit;

    // The size first: the download itself has no limit until it is complete
    Response info = getFollowing(repoPath(ref), "application/vnd.github+json", "application/vnd.github+json");
    if (info.code != HTTP_OK)
    {
        out.error = failure(info);
        return out;
    }
    boost::system::error_code ec;
    const boost::json::value json = boost::json::parse(info.body, ec);
    if (ec || !json.is_object())
    {
        out.error = "GitHub sent an unexpected answer.";
        return out;
    }
    const LLSD repo = LlsdFromJson(json);
    if (repo.has("size") && repo["size"].asInteger() > MAX_REPO_KB)
    {
        out.error = llformat("The repository is too large (%d MB).", repo["size"].asInteger() / 1024);
        return out;
    }

    Response response = getFollowing(apiPath(ref, "zipball", commit), "application/vnd.github+json",
        "application/zip");
    if (response.code != HTTP_OK)
    {
        out.error = failure(response);
        return out;
    }
    if (response.body.size() > MAX_ZIP_BYTES)
    {
        out.error = "The download is over 50 MB.";
        return out;
    }
    out.ok = true;
    out.body = std::move(response.body);
    return out;
}
}
