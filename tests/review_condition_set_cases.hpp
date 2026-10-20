// Process-only conditions persistence contracts; expected values are chosen
// independently of the saved document and checked against direct evaluation.
void ReviewConditionSetTests(const fs::path& eval,const fs::path& root,const fs::path& inspector,
    const fs::path& script,const fs::path& pwsh,const fs::path& report,const fs::path& evidence) {
    const auto evidence_tag=L"-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L".json";
    const auto common=[&](const fs::path& output) {
        return std::vector<std::wstring>{L"-NoProfile",L"-File",script.native(),L"-Report",report.native(),
            L"-Inspector",inspector.native(),L"-Evaluator",eval.native(),L"-ProductRoot",(root/"product").native(),
            L"-OutputDirectory",output.native(),L"-ConditionsRoot",root.native()};
    };
    const auto load=[&](const fs::path& file,const fs::path& output,const fs::path& selected_root=fs::path{}) {
        auto args=common(output);if(!selected_root.empty())args.back()=selected_root.native();
        args.insert(args.end(),{L"-ConditionsFile",file.native()});return Run(pwsh,root,args);
    };
    fs::path saved;
    for(bool color:{false,true}) {
        const auto output=root/(color?"conditions-color":"conditions-geometry");auto args=common(output);
        args.insert(args.end(),{L"-SaveConditions",L"-ProfileFile",(root/"draft.json").native(),L"-CorrectionJob",(root/"job.json").native(),
            L"-LeftUm",L"565000",L"-TopUm",L"400000",L"-RightUm",L"575000",L"-BottomUm",L"408000",
            L"-Dpi",L"100",L"-WidthPixels",L"40",L"-HeightPixels",L"32",L"-Resampling",L"bilinear"});
        if(color)args.insert(args.end(),{L"-ColorJob",(root/"color-job.json").native()});
        const auto result=Run(pwsh,root,args);
        Require(result.exit==0&&result.error.empty(),"explicit save succeeds: "+result.error+result.out);
        const auto file=output/"evaluation-conditions.json";const auto record=Parse(file);
        String(record,"schemaVersion","a0.saved-review-evaluation-conditions.v1");
        String(record,"reviewReportSha256",Sha(report));
        String(At(record,"profile"),"relativePath","draft.json");String(At(record,"profile"),"sha256",Sha(root/"draft.json"));
        const auto raster=At(record,"raster");Number(raster,"leftUm",565000);Number(raster,"dpi",100);Number(raster,"widthPixels",40);
        Require(!At(record,"productApproved").boolean,"saved selection cannot approve a profile");
        const auto before=Sha(file);const auto restored=root/(color?"conditions-color-restored":"conditions-geometry-restored");
        const auto reloaded=load(file,restored);
        Require(reloaded.exit==0&&reloaded.error.empty(),"separate process reload succeeds: "+reloaded.error+reloaded.out);
        const auto direct=root/(color?"color-direct":"geometry-direct");
        Require(Sha(restored/"evaluation.jpg")==Sha(direct/"evaluation.jpg"),"restored parameters match independently invoked evaluation");
        const auto binding=Parse(restored/"review-evaluation-binding.json");
        String(binding,"schemaVersion","a0.saved-review-evaluation-binding.v2");
        String(At(binding,"conditionSet"),"sha256",before);
        Require(Sha(file)==before,"reading preserves original saved conditions");
        const auto previous_binding=Sha(restored/"review-evaluation-binding.json");
        Require(load(file,restored).exit==2&&Sha(restored/"review-evaluation-binding.json")==previous_binding,"saved mode does not replace existing results");
        const auto text=Text(file);Require(text.find(":/")==std::string::npos&&text.find(":\\")==std::string::npos&&text.find("\\\\")==std::string::npos,"conditions have no absolute paths");
        if(!color)saved=file;
        Evidence(evidence,file,std::wstring(color?L"saved-color-conditions":L"saved-geometry-conditions")+evidence_tag);
    }
    const auto valid=Text(saved);const auto damaged=root/"damaged-conditions.json";
    const auto bad=[&](std::string text,const char* reason) {
        WriteText(damaged,text);const auto output=root/("conditions-invalid-"+std::to_string(checks));const auto result=load(damaged,output);
        Require(result.exit==2&&!fs::exists(output),std::string("reject before creating output: ")+reason+" "+result.error);
    };
    bad(Replace(valid,"\"dpi\":100","\"dpi\":100.0"),"fractional integer representation");
    bad(Replace(valid,"\"dpi\":100","\"dpi\":1e2"),"exponent integer representation");
    bad(Replace(valid,"\"dpi\":100","\"dpi\":100,\"dpi\":100"),"duplicate nested member");
    bad(Replace(valid,"\"dpi\":100","\"dpi\":100,\"command\":\"anything\""),"unknown member");
    bad(Replace(valid,"\"dpi\":100","\"dpi\":0"),"zero DPI");
    bad(Replace(valid,"\"widthPixels\":40","\"widthPixels\":41"),"wrong ceil raster");
    bad(Replace(valid,"\"productApproved\":false","\"productApproved\":true"),"approval claim");
    bad(Replace(valid,"a0.saved-review-evaluation-conditions.v1","a0.saved-review-evaluation-conditions.v2"),"unknown schema");
    bad(Replace(valid,"synthetic-review-352","other-pair"),"different pair");
    bad(Replace(valid,Sha(report),std::string(64,'0')),"different review snapshot");
    for(const auto* path:{"../draft.json","/draft.json","C:/draft.json","folder/../draft.json","folder//draft.json","draft.json ","draft.json:stream","missing.json","NUL.json","COM1"})
        bad(Replace(valid,"\"relativePath\":\"draft.json\"",std::string("\"relativePath\":\"")+path+"\""),"unsafe or missing reference");
    bad(Replace(valid,"\"sha256\":\""+Sha(root/"draft.json"),"\"sha256\":\""+std::string(64,'0')),"profile digest mismatch");
    const auto profile=Text(root/"draft.json");WriteText(root/"draft.json",profile+" ");
    bad(valid,"changed profile bytes");WriteText(root/"draft.json",profile);
    const auto job=Text(root/"job.json");WriteText(root/"job.json",job+" ");
    bad(valid,"changed job bytes");WriteText(root/"job.json",job);
    const auto color_job=Text(root/"color-job.json");WriteText(root/"color-job.json",color_job+" ");
    bad(Text(root/"conditions-color/evaluation-conditions.json"),"changed color job bytes");WriteText(root/"color-job.json",color_job);
    Junction(root/"conditions-alias",root/"conditions-geometry");
    const auto output=root/"conditions-reparse";
    Require(load(root/"conditions-alias/evaluation-conditions.json",output).exit==2&&!fs::exists(output),"reparse conditions file rejected");
    const auto relocated=root/"relocated-conditions";fs::create_directory(relocated);
    fs::copy_file(root/"draft.json",relocated/"draft.json");fs::copy_file(root/"job.json",relocated/"job.json");
    fs::copy_file(saved,relocated/"chosen.json");
    Require(load(relocated/"chosen.json",root/"explicit-relocation",relocated).exit==0,"explicit root relocation accepts only the same bytes");
    Require(fs::is_empty(root/"product"),"saved conditions never write product storage");
}
