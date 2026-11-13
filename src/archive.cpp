#include "archive.h"
#include "common.h"
#include "symbolizer.h"

#include <fstream>
#include <sstream>
#include <map>
#include <set>

namespace wperf {
bool LoadArchiveModules(const std::filesystem::path& directory, TraceData& trace, std::wstring* error) {
    const auto fail=[&](const wchar_t* message) { if(error)*error=message; return false; };
    std::error_code ec;
    const auto root=std::filesystem::weakly_canonical(directory,ec);
    if(ec) return fail(L"invalid archive directory");
    const auto map_path=root/L"modules.tsv";
    const auto bytes=std::filesystem::file_size(map_path,ec);
    if(ec || bytes>16*1024*1024) return fail(L"archive modules.tsv is missing or too large");
    std::ifstream file(map_path,std::ios::binary);
    std::string line;
    if(!std::getline(file,line) || line!="wperf-archive-map-v1") return fail(L"unsupported archive map");
    std::map<std::wstring,std::wstring> mapping;
    while(std::getline(file,line)) {
        std::istringstream fields(line);
        std::string path_text, relative_text, hash_text;
        uint64_t size=0; uint32_t stamp=0, checksum=0;
        if(!(fields>>path_text>>stamp>>checksum>>size>>relative_text>>hash_text)) return fail(L"malformed archive map entry");
        std::wstring original,relative;
        if(!UnhexText(path_text,original) || !UnhexText(relative_text,relative)) return fail(L"invalid archive path encoding");
        const auto candidate=std::filesystem::weakly_canonical(root/relative,ec);
        if(ec || !std::filesystem::path(relative).is_relative()) return fail(L"invalid archive module path");
        const auto within=candidate.lexically_relative(root);
        if(within.empty() || *within.begin()==L"..") return fail(L"archive module escapes its directory");
        ImageRecord image; image.path=original; image.timestamp=stamp; image.checksum=checksum; image.size=size;
        if(!ModuleIdentityMatches(image,ReadModuleIdentity(candidate.wstring())) ||
           FileSha256(candidate)!=ToUtf16(hash_text)) return fail(L"archive module identity/hash mismatch");
        mapping[ModuleIdentityKey(image)]=candidate.wstring();
    }
    for(auto& image:trace.images) {
        const auto found=mapping.find(ModuleIdentityKey(image));
        if(found!=mapping.end()) image.resolved_path=found->second;
    }
    return true;
}
std::wstring ArchiveSymbolPath(const std::filesystem::path& directory, const TraceData& trace) {
    std::set<std::wstring> paths;
    paths.insert(L"srv*"+std::filesystem::absolute(directory/L"symbols").wstring());
    for(const auto& image:trace.images) if(!image.resolved_path.empty())
        paths.insert(std::filesystem::path(image.resolved_path).parent_path().wstring());
    std::wstring result;
    for(const auto& path:paths) { if(!result.empty())result+=L";"; result+=path; }
    return result;
}
}
