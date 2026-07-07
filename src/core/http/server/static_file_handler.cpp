#include <filesystem>
#include <format>
#include <system_error>

#include <boost/url/parse.hpp>

#include "http/server/static_file_handler.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::HTTP
{

    StaticFileHandler::StaticFileHandler(std::string_view prefix, const std::filesystem::path& doc_root) :
        StaticFileHandler(boost::urls::parse_uri_reference(prefix).value(), doc_root)
    {
    }

    StaticFileHandler::StaticFileHandler(boost::urls::url prefix, const std::filesystem::path& doc_root) :
        m_Prefix(std::move(prefix)),
        m_DocRoot(std::filesystem::absolute(doc_root))
    {
        if (std::error_code ec; !std::filesystem::is_directory(m_DocRoot, ec))
        {
            LogWarning(Channel::Web, std::format("Specified static directory '{}' was invalid; error was -> {}", m_DocRoot.string(), ec.message()));
        }

        // Canonicalise the fixed document root once at construction.  The root is
        // operator-supplied (not client-controlled) and never changes, so the
        // per-request canonicalisation in match() is redundant work; resolving it
        // here also lets the path jail compare against a stable, normalised root.
        std::error_code canon_ec;
        auto canonical_root = std::filesystem::weakly_canonical(m_DocRoot, canon_ec);
        if (canon_ec)
        {
            LogWarning(Channel::Web, std::format("Failed to canonicalise static directory '{}'; error was -> {}", m_DocRoot.string(), canon_ec.message()));
        }
        else
        {
            m_DocRoot = std::move(canonical_root);
        }
    }

    bool StaticFileHandler::match(const boost::urls::url_view& target, std::filesystem::path& result)
    {
        if (boost::system::result<boost::urls::url> parsed_target = boost::urls::parse_uri_reference(target); parsed_target.has_error())
        {
            LogDebug(Channel::Web, std::format("Failed to parse target URL as a static asset; error was {}", parsed_target.error().message()));
        }
        else if (auto prefix_len = match_prefix(parsed_target.value().normalize_path().segments(), static_cast<boost::urls::url_view>(m_Prefix).segments()); prefix_len >= 0)
        {
            result = m_DocRoot;

            // Build the path from the SAME (normalized) segment sequence that
            // match_prefix counted prefix_len against. Using the raw target.segments()
            // here while prefix_len was computed from the normalized path skews the
            // advance when the raw path carries segments that normalize away ("//",
            // "/./", "/../"), which could append a prefix segment or resolve the wrong
            // file within the doc-root. (parsed_target was normalize_path()'d above.)
            auto segs = parsed_target.value().segments();
            auto it = segs.begin();
            auto end = segs.end();

            // Skip only the non-empty prefix segments that were actually matched
            std::advance(it, prefix_len);

            while (it != end)
            {
                if (auto seg = *it; !seg.empty())
                {
                    result.append(seg.begin(), seg.end());
                }
                ++it;
            }

            // If result points to a directory, serve index.html
            if (std::filesystem::is_directory(result))
            {
                result /= "index.html";
            }

            // Security: Verify resolved path is within document root (path jail).
            // m_DocRoot is already canonicalised at construction.
            std::error_code canon_ec;
            auto canonical_result = std::filesystem::weakly_canonical(result, canon_ec);

            if (canon_ec)
            {
                LogWarning(Channel::Web, std::format("Path canonicalization failed: {}", canon_ec.message()));
                return false;
            }

            // Compare on PATH COMPONENTS, not a raw string prefix.  A string-prefix
            // test ("/srv/www" being a prefix of "/srv/wwwsecret/x") would let a
            // sibling directory whose name merely starts with the root escape the
            // jail.  lexically_relative() yields the route from the root to the
            // resolved path in component terms; the result must stay inside the
            // root, i.e. it must not be empty and must not begin with a "..".
            if (const auto relative = canonical_result.lexically_relative(m_DocRoot); relative.empty() || relative.begin()->native() == std::filesystem::path("..").native())
            {
                LogWarning(Channel::Web, std::format("Path traversal attempt blocked: {} escapes {}", canonical_result.string(), m_DocRoot.string()));
                return false;
            }

            return true;
        }

        return false;
    }

    int StaticFileHandler::match_prefix(boost::urls::segments_view target, boost::urls::segments_view prefix)
    {
        // Count the non-empty segments in the prefix (root "/" has one empty segment which we skip)
        std::size_t non_empty_prefix_count = 0;
        for (const auto& seg : prefix)
        {
            if (!seg.empty())
            {
                ++non_empty_prefix_count;
            }
        }

        // A root prefix "/" matches everything
        if (non_empty_prefix_count == 0)
        {
            return 0;
        }

        // Trivially reject target that cannot contain the prefix
        if (auto target_size = target.size(); target_size < non_empty_prefix_count)
        {
            return -1;
        }

        // Match the non-empty prefix segments against target segments
        auto it_target = target.begin();
        auto end_target = target.end();
        auto it_prefix = prefix.begin();
        auto end_prefix = prefix.end();
        int matched = 0;

        while (it_target != end_target && it_prefix != end_prefix)
        {
            // Skip empty segments in both
            if ((*it_prefix).empty())
            {
                ++it_prefix;
                continue;
            }
            if ((*it_target).empty())
            {
                ++it_target;
                ++matched;
                continue;
            }

            if (*it_target != *it_prefix)
            {
                return -1;
            }

            ++it_target;
            ++it_prefix;
            ++matched;
        }

        // Check all non-empty prefix segments were consumed
        while (it_prefix != end_prefix)
        {
            if (!(*it_prefix).empty())
            {
                return -1;
            }
            ++it_prefix;
        }

        return matched;
    }

}
// namespace AqualinkAutomate::HTTP
