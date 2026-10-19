#include "a0/m2/stitch_correction_job.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <type_traits>

using namespace a0::m2::correction;
namespace {
int checks=0;
void Check(bool ok,const char* why) { ++checks; if(!ok)throw std::runtime_error(why); }
std::string Read(const std::filesystem::path& p) {
    std::ifstream in(p,std::ios::binary); if(!in)throw std::runtime_error("fixture read failed");
    return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}
std::string Replace(std::string s,const std::string& from,const std::string& to) {
    const auto at=s.find(from); if(at==std::string::npos)throw std::runtime_error("mutation not found");
    s.replace(at,from.size(),to); return s;
}
void Bad(const std::string& s) {
    bool threw=false;try{(void)ParseStitchCorrectionJob(s);}catch(const std::invalid_argument&){threw=true;}
    Check(threw,"invalid versioned policy must be refused");
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("fixture directory required");
        const auto root=std::filesystem::path(argv[1]);
        const auto text=Read(root/"refinement-job.json");
        const auto job = ParseStitchCorrectionJob(text);
        Check(job.source_json==text && job.refinement && job.refinement->levels==7,"v2 retains exact source and explicit levels");
        const auto legacy=ParseStitchCorrectionJob(Read(root/"job.json"));
        Check(!legacy.refinement,"v1 never implicitly enables refinement");
        for(const auto n:{1u,8u})Check(ParseStitchCorrectionJob(Replace(text,"\"levels\": 7","\"levels\": "+std::to_string(n))).refinement->levels==n,"level bounds accepted exactly");
        for(const auto value:{"0","9","4294967295","4294967296","7.0","7e0","-0","-1","true","null","\"7\"","1e999"})
            Bad(Replace(text,"\"levels\": 7",std::string("\"levels\": ")+value));
        Bad(Replace(text,"\"levels\": 7","\"other\": 7"));
        Bad(Replace(text,"\"levels\": 7","\"levels\": 7,\"other\": 1"));
        Bad(Replace(text,"\"levels\": 7","\"levels\": 7,\"levels\": 7"));
        Bad(Replace(text,"\"levels\": 7",""));
        {auto null_refinement=text;const auto begin=null_refinement.find('{',null_refinement.find("\"refinement\"")),end=null_refinement.find('}',begin);
            null_refinement.replace(begin,end-begin+1,"null");Bad(null_refinement);}
        Bad(Replace(text,"a0.stitch-correction-job.v2","a0.stitch-correction-job.v1"));
        Bad(Replace(text,"a0.stitch-correction-job.v2","a0.stitch-correction-job.v3"));
        Bad(Replace(Read(root/"job.json"),"a0.stitch-correction-job.v1","a0.stitch-correction-job.v2"));
        Bad(text+"{}");Bad(std::string(65537,' '));Bad(std::string(9,'[')+std::string(9,']'));
        Check(ParseStitchCorrectionJob(" \n"+text+"\t").source_json==" \n"+text+"\t","whitespace snapshot remains exact");
        const std::string hash(64,'a');
        const CorrectionReportBindings binding{{{{hash,1},{hash,1}}},hash,hash,hash,{{0,0,1000,1000},254,10,10}};
        AutoImagePatchCorrectionResult result{AutoImagePatchOutcome::SelectionRejected,
            {PatchSeedSelectionOutcome::InsufficientSeeds,DocumentCorrectionOutcome::InsufficientPoints,{},{}},{}};
        const auto report=StitchCorrectionReport(job,result,binding),old=StitchCorrectionReport(legacy,result,binding);
        Check(report.find("a0.stitch-correction-report.v2")!=std::string::npos && report.find(text)!=std::string::npos,"v2 report includes versioned exact policy on early selection rejection");
        Check(old.find("a0.stitch-correction-report.v1")!=std::string::npos && old.find("refinement")==std::string::npos,"v1 diagnostic has no refinement extension");
        auto bad_report=result;
        bad_report.correction=ImagePatchCorrectionResult{ImagePatchCorrectionOutcome::MatchingFailed,{}, {}};
        bad_report.correction->points.push_back({{0,0},ImagePatchOutcome::Ambiguous,{ImagePatchOutcome::Matched,{},{},{},{},{},0,0},{},{},{}});
        auto RefuseReport=[&](const StitchCorrectionJob& policy) {
            bool threw=false;try{(void)StitchCorrectionReport(policy,bad_report,binding);}catch(const std::invalid_argument&){threw=true;}
            Check(threw,"inconsistent refinement diagnostics cannot be serialized as a valid versioned report");
        };
        RefuseReport(job); // matched search cannot omit required levels
        auto& search=bad_report.correction->points[0].forward;search.outcome=ImagePatchOutcome::Ambiguous;
        search.refinement.push_back({.0625,ImagePatchOutcome::Ambiguous,{},{},{},{},9,9});
        RefuseReport(legacy); // v1 cannot silently discard refinement diagnostics
        search.refinement[0].examined_candidates=10;RefuseReport(job);
        static_assert(!std::is_default_constructible_v<StitchCorrectionJob>);
        static_assert(std::is_const_v<decltype(job.refinement)>);
        std::cout << "stitch_correction_job checks=" << checks << " failures=0\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
