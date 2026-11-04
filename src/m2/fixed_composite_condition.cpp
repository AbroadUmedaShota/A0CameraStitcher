#include "a0/m2/experimental/fixed_composite_condition.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <iterator>

namespace a0::m2::experimental {
namespace {
std::runtime_error IoError(const char* what){
    return std::runtime_error(std::string("fixed composite condition ")+what+" (error "+std::to_string(GetLastError())+")");
}
void RemoveOwnPartial(const std::filesystem::path& partial){
    std::error_code ignored;std::filesystem::remove(partial,ignored);
}
}

void WriteFixedCompositeCondition(const std::filesystem::path& file,const FixedCompositeCondition& condition){
    // Format validates; the file ends with a newline.
    const auto text=FormatFixedCompositeCondition(condition)+"\n";
    std::error_code error;
    const auto parent=file.parent_path();
    if(file.filename().empty() || (!parent.empty() && !std::filesystem::is_directory(parent,error)))
        throw std::invalid_argument("fixed composite condition folder does not exist");
    const auto existing=std::filesystem::symlink_status(file,error);
    if(existing.type()!=std::filesystem::file_type::not_found) // MSVC also sets `error` for a missing file
        throw std::invalid_argument("fixed composite condition already exists; never overwrite");
    auto partial=file;partial+=L".partial";
    // Exclusive: a partial that is already there is somebody else's, not ours to reuse.
    const HANDLE handle=CreateFileW(partial.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE){
        if(GetLastError()==ERROR_FILE_EXISTS || GetLastError()==ERROR_ALREADY_EXISTS)
            throw std::invalid_argument("fixed composite condition partial already exists");
        throw IoError("partial cannot be created");
    }
    bool open=true;
    try{
        DWORD written{};
        if(!WriteFile(handle,text.data(),static_cast<DWORD>(text.size()),&written,nullptr) || written!=text.size())throw IoError("write failed");
        if(!FlushFileBuffers(handle))throw IoError("flush failed");
        open=false;if(!CloseHandle(handle))throw IoError("close failed");
    }catch(...){
        if(open)CloseHandle(handle);RemoveOwnPartial(partial);throw;
    }
    try{
        std::string reread;
        { // closed before the move
            std::ifstream in(partial,std::ios::binary);
            reread.assign(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());
        }
        if(reread!=text || FormatFixedCompositeCondition(ParseFixedCompositeCondition(reread))+"\n"!=text)
            throw std::runtime_error("fixed composite condition partial changed");
        // No MOVEFILE_REPLACE_EXISTING: an existing target makes the move fail.
        if(!MoveFileExW(partial.c_str(),file.c_str(),MOVEFILE_WRITE_THROUGH))
            throw IoError("cannot be moved into place without replacing");
    }catch(...){
        RemoveOwnPartial(partial);throw;
    }
}
}
