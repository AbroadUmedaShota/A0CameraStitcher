#pragma once
#include "a0/phase0/experimental/fixed_composite_tool_v3.hpp"
#include <chrono>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace a0::phase0::experimental {
// The command line of StillReview that makes and revises the fixed composite condition v3 (#417; the tool is fixed_composite_tool_v3.hpp,
// the design is docs/design/fixed-composite-registration.md 11.19 and 11.20, the user's side is docs/WPD_STILL_REVIEW.md). It opens no
// window, never touches a camera, never overwrites or removes a file, and has no default for any range, line, limit, region, DPI or design
// constant: what is not given is not made.
//
//   StillReview --make-fixed-composite-condition-v3 <kind> --settings-folder <folder> [kind arguments] [layer 2 arguments] [--dry-run]
//
//   kind created            --profile <draft rig profile> --region-um <left,top,right,bottom> --dpi <integer>
//   kind migrated-from-v2   --v2-condition <the v2 condition file, after a person moved it out of the settings folder's way>
//   kind values             (none: the policy file is read again)
//   kind manual-adjustment  --seam-dx-px <number> --seam-dy-px <number> --rotation-deg <number>
//   kind rollback           --restore-sequence <revision number>
//   kind saved-estimate     --report <saved report>   (refused until the capture lane has a reader of the report v8)
//   layer 2                 --layer-two-falloff-px <L> --layer-two-max-strain <s_max>   (both or neither; needed when the policy enables layer 2)
//
// Exit code: made 0, a dry run whose checks passed 0, not made 1, wrong arguments 2. The output has no user path and no machine identifier:
// the revision file is named by its file name only, and the facts are the revision, the kind and the SHA-256 of the record and of the policy.
inline constexpr std::wstring_view kFixedConditionV3CommandName=L"--make-fixed-composite-condition-v3";
inline constexpr std::string_view kFixedConditionV3Usage=
    "usage: StillReview --make-fixed-composite-condition-v3 <kind> --settings-folder <folder> [--layer-two-falloff-px <L> --layer-two-max-strain <s_max>] [--dry-run]\n"
    "  kind created           --profile <draft rig profile> --region-um <left,top,right,bottom> --dpi <integer>\n"
    "  kind migrated-from-v2  --v2-condition <v2 condition file that was moved out of the settings folder>\n"
    "  kind values            (no further argument: the policy file is read again)\n"
    "  kind manual-adjustment --seam-dx-px <number> --seam-dy-px <number> --rotation-deg <number>\n"
    "  kind rollback          --restore-sequence <revision number>\n"
    "  kind saved-estimate    --report <saved report>  (not available yet)\n";

struct FixedConditionV3CliParse {
    std::optional<FixedConditionV3Request> request;
    std::string error;   // ASCII: why the arguments were refused; empty when they were accepted
};

namespace fixed_cli_v3_detail {
inline std::optional<std::string> Ascii(const std::wstring& text){
    std::string out;out.reserve(text.size());
    for(const wchar_t c:text){if(c<0x20 || c>0x7e)return std::nullopt;out.push_back(static_cast<char>(c));}
    return out;
}
// A finite number written the way std::to_chars writes it (no leading plus, no spaces, nothing after the number).
inline std::optional<double> FiniteNumber(const std::wstring& text){
    const auto ascii=Ascii(text);
    if(!ascii || ascii->empty())return std::nullopt;
    double value{};
    const auto r=std::from_chars(ascii->data(),ascii->data()+ascii->size(),value);
    if(r.ec!=std::errc{} || r.ptr!=ascii->data()+ascii->size() || !std::isfinite(value))return std::nullopt;
    return value;
}
inline std::optional<std::uint64_t> Whole(const std::wstring& text,std::size_t maximum_digits){
    if(text.empty() || text.size()>maximum_digits || text.find_first_not_of(L"0123456789")!=std::wstring::npos)return std::nullopt;
    std::uint64_t n=0;for(const wchar_t c:text)n=n*10+static_cast<std::uint64_t>(c-L'0');
    return n;
}
}

// `args` are the arguments after the command name: the kind, then flags. Every flag is given once, only the flags of the kind are accepted,
// and every flag a kind needs must be there.
[[nodiscard]] inline FixedConditionV3CliParse ParseFixedConditionV3Arguments(const std::vector<std::wstring>& args){
    namespace m2=a0::m2::experimental;
    using Kind=m2::FixedCompositeRevisionKind;
    using fixed_cli_v3_detail::FiniteNumber;using fixed_cli_v3_detail::Whole;
    FixedConditionV3CliParse out;
    const auto refuse=[&](std::string why){out.request.reset();out.error=std::move(why);return out;};
    if(args.empty())return refuse("the kind is missing");
    const auto kind_text=fixed_cli_v3_detail::Ascii(args[0]);
    const auto kind=kind_text?m2::ParseFixedCompositeRevisionKind(*kind_text):std::nullopt;
    if(!kind)return refuse("the kind is not one of created, migrated-from-v2, values, manual-adjustment, rollback, saved-estimate");

    FixedConditionV3Request request;request.kind=*kind;
    std::optional<double> dx,dy,rotation,falloff,strain;
    bool have_folder=false,have_profile=false,have_region=false,have_dpi=false,have_v2=false,have_restore=false,have_report=false,dry_run=false;
    for(std::size_t i=1;i<args.size();++i){
        const std::wstring_view flag(args[i]);
        if(flag==L"--dry-run"){if(dry_run)return refuse("--dry-run is given twice");dry_run=true;continue;}
        if(i+1>=args.size())return refuse("a flag has no value");
        const std::wstring& value=args[++i];
        const auto once=[&](bool& seen)->bool{if(seen)return false;seen=true;return true;};
        const auto number=[&](std::optional<double>& slot)->bool{if(slot)return false;slot=FiniteNumber(value);return slot.has_value();};
        const bool first_kind=*kind==Kind::created;
        if(flag==L"--settings-folder"){if(!once(have_folder) || value.empty())return refuse("--settings-folder is given twice or empty");request.settings_folder=std::filesystem::path(value);}
        else if(flag==L"--layer-two-falloff-px"){if(!number(falloff) || !(*falloff>0))return refuse("--layer-two-falloff-px is not a positive number or is given twice");}
        else if(flag==L"--layer-two-max-strain"){if(!number(strain) || !(*strain>0))return refuse("--layer-two-max-strain is not a positive number or is given twice");}
        else if(flag==L"--profile" && first_kind){if(!once(have_profile) || value.empty())return refuse("--profile is given twice or empty");request.profile=std::filesystem::path(value);}
        else if(flag==L"--region-um" && first_kind){
            if(!once(have_region))return refuse("--region-um is given twice");
            std::size_t at=0;
            for(int k=0;k<4;++k){
                const auto end=k<3?value.find(L',',at):value.size();
                if(end==std::wstring::npos || end<=at)return refuse("--region-um is not left,top,right,bottom");
                const auto n=Whole(value.substr(at,end-at),12);
                if(!n)return refuse("--region-um holds something that is not a whole number of micrometres");
                request.region_um[static_cast<std::size_t>(k)]=static_cast<long long>(*n);at=end+1;
            }
        }else if(flag==L"--dpi" && first_kind){
            const auto n=Whole(value,5);if(!once(have_dpi) || !n)return refuse("--dpi is not a whole number or is given twice");
            request.dpi=static_cast<std::uint32_t>(*n);
        }else if(flag==L"--v2-condition" && *kind==Kind::migrated_from_v2){
            if(!once(have_v2) || value.empty())return refuse("--v2-condition is given twice or empty");request.v2_condition=std::filesystem::path(value);
        }else if(flag==L"--seam-dx-px" && *kind==Kind::manual_adjustment){if(!number(dx))return refuse("--seam-dx-px is not a number or is given twice");}
        else if(flag==L"--seam-dy-px" && *kind==Kind::manual_adjustment){if(!number(dy))return refuse("--seam-dy-px is not a number or is given twice");}
        else if(flag==L"--rotation-deg" && *kind==Kind::manual_adjustment){if(!number(rotation))return refuse("--rotation-deg is not a number or is given twice");}
        else if(flag==L"--restore-sequence" && *kind==Kind::rollback){
            const auto n=Whole(value,6);if(!once(have_restore) || !n || *n<1)return refuse("--restore-sequence is not a revision number or is given twice");
            request.restore_sequence=static_cast<std::uint32_t>(*n);
        }else if(flag==L"--report" && *kind==Kind::saved_estimate){
            if(!once(have_report) || value.empty())return refuse("--report is given twice or empty");request.report=std::filesystem::path(value);
        }else return refuse("a flag is unknown, or does not belong to this kind");
    }
    if(!have_folder)return refuse("--settings-folder is missing");
    if(falloff.has_value()!=strain.has_value())return refuse("--layer-two-falloff-px and --layer-two-max-strain are given together or not at all");
    if(falloff)request.layer_two=m2::LayerTwoStrainDesignV3{*falloff,*strain};
    switch(*kind){
    case Kind::created:if(!have_profile || !have_region || !have_dpi)return refuse("created needs --profile, --region-um and --dpi");break;
    case Kind::migrated_from_v2:if(!have_v2)return refuse("migrated-from-v2 needs --v2-condition");break;
    case Kind::manual_adjustment:
        if(!dx || !dy || !rotation)return refuse("manual-adjustment needs --seam-dx-px, --seam-dy-px and --rotation-deg");
        request.entered=m2::FixedCompositeRevisionChangeV3::Entered{*dx,*dy,*rotation};break;
    case Kind::rollback:if(!have_restore)return refuse("rollback needs --restore-sequence");break;
    case Kind::saved_estimate:if(!have_report)return refuse("saved-estimate needs --report");break;
    case Kind::values:break;
    }
    request.dry_run=dry_run;
    out.request=std::move(request);
    return out;
}

// Prints a line every `interval` while the creation checks run (they take minutes at a real raster), so that a long wait can be told from a
// hang. interval 0: no thread, only the first notice.
class CreationCheckProgress {
public:
    CreationCheckProgress(std::ostream& out,std::chrono::seconds interval):out_(out),interval_(interval){}
    CreationCheckProgress(const CreationCheckProgress&)=delete;
    CreationCheckProgress& operator=(const CreationCheckProgress&)=delete;
    ~CreationCheckProgress(){Stop();}
    void Start(){
        if(started_)return;
        started_=true;begin_=std::chrono::steady_clock::now();
        out_<<"check_started=1 note=範囲の端の姿勢をすべて調べています。実際の大きさの領域では数分かかることがあります。終わるまでお待ちください\n"<<std::flush;
        if(interval_.count()>0)worker_=std::thread([this]{
            std::unique_lock lock(mutex_);
            while(!stop_ && !wake_.wait_for(lock,interval_,[this]{return stop_;}))
                out_<<"check_running seconds="<<std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-begin_).count()<<'\n'<<std::flush;
        });
    }
    void Stop(){
        { std::lock_guard lock(mutex_);stop_=true; }
        wake_.notify_all();
        if(worker_.joinable())worker_.join();
    }
    [[nodiscard]] long long Seconds() const{return started_?std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-begin_).count():0;}
private:
    std::ostream& out_;
    std::chrono::seconds interval_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    std::chrono::steady_clock::time_point begin_{};
    bool started_{},stop_{};
};

// Runs the command (`args` are the arguments after the command name) and writes its report to `out`. Returns the exit code.
[[nodiscard]] inline int RunFixedConditionV3Command(const std::vector<std::wstring>& args,std::ostream& out,const a0::m2::experimental::FixedCompositeSha256& hash,
    std::chrono::system_clock::time_point now,std::chrono::seconds progress_interval=std::chrono::seconds(30)){
    const auto parsed=ParseFixedConditionV3Arguments(args);
    if(!parsed.request){
        out<<"fixed_composite_condition_v3_made=0 camera_operations=0\nerror="<<parsed.error<<'\n'<<kFixedConditionV3Usage<<std::flush;
        return 2;
    }
    auto request=*parsed.request;
    CreationCheckProgress progress(out,progress_interval);
    request.on_stage=[&progress](std::string_view stage){if(stage=="check")progress.Start();};
    FixedConditionV3MakeResult made;
    try{made=MakeFixedCompositeConditionV3File(request,hash,now);}
    catch(const std::filesystem::filesystem_error&){progress.Stop();out<<"fixed_composite_condition_v3_made=0 camera_operations=0\nファイルの操作に失敗しました。何も変えていません\n"<<std::flush;return 1;}
    catch(const std::exception& e){progress.Stop();out<<"fixed_composite_condition_v3_made=0 camera_operations=0\n"<<e.what()<<'\n'<<std::flush;return 1;}
    progress.Stop();
    out<<"fixed_composite_condition_v3_made="<<made.made<<" checked="<<made.checked<<" dry_run="<<request.dry_run<<" camera_operations=0\n"<<made.message<<'\n';
    if(made.checked){
        out<<"revision="<<made.revision<<" kind="<<made.kind<<'\n';
        if(made.made)out<<"revision_file="<<made.file.filename().string()<<'\n';
        if(!made.condition_sha256.empty())out<<"condition_sha256="<<made.condition_sha256<<'\n';
        if(!made.parent_sha256.empty())out<<"parent_sha256="<<made.parent_sha256<<'\n';
        out<<"policy_sha256="<<made.policy_sha256<<'\n';
        out<<"check_seconds="<<progress.Seconds()<<'\n';
        for(const auto& line:made.details)if(!line.starts_with("revision="))out<<line<<'\n';
    }
    out<<std::flush;
    return (made.made || (made.checked && request.dry_run))?0:1;
}
}
