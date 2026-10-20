// Included by the existing process-only harness. Uses its independent WIC,
// SHA and child-process helpers; no production review/renderer is linked.
void ReviewEvaluationBridgeTests(const fs::path& fixtures,const fs::path& eval,const fs::path& parent,
    const fs::path& inspector,const fs::path& script,const fs::path& pwsh) {
    const auto root=parent/L"review evaluation \u65e5\u672c\u8a9e";
    fs::create_directory(root);fs::create_directory(root/"product");
    Synthetic(root/"CAM-A/original.jpg",7360,4912,2);
    Synthetic(root/"CAM-B/original.jpg",7360,4912,6);
    WriteText(root/"draft.json",Replace(Text(fixtures/"calibrated-draft.json"),"9400.0","4800.0"));
    auto geometry=Text(fixtures.parent_path()/"stitch-correction/job.json");
    geometry=Replace(geometry,"\"gridColumns\": 5","\"gridColumns\": 16");
    geometry=Replace(geometry,"\"gridRows\": 4","\"gridRows\": 12");
    geometry=Replace(geometry,"\"maximumSeeds\": 6","\"maximumSeeds\": 32");
    geometry=Replace(geometry,"\"minimumSpacingMm\": 1","\"minimumSpacingMm\": 0.5");
    WriteText(root/"job.json",geometry);
    WriteText(root/"color-job.json",Text(fixtures.parent_path()/"stitch-correction/color-job.json"));
    const auto a=root/"CAM-A/original.jpg",b=root/"CAM-B/original.jpg";
    const auto file=[](const fs::path& path){
        const auto utf8=path.generic_u8string();
        return std::string("{\"path\":\"")+a0::common::protocol_json::JsonEscape(
            std::string_view(reinterpret_cast<const char*>(utf8.data()),utf8.size()))+
            "\",\"sha256\":\""+Sha(path)+"\",\"bytes\":"+std::to_string(fs::file_size(path))+"}";
    };
    fs::create_directory(root/"review");
    Synthetic(root/"review/uncalibrated-review.jpg",11040,4912,0);
    auto input_a=file(a),input_b=file(b);
    input_a.insert(1,"\"alias\":\"CAM-A\",");input_b.insert(1,"\"alias\":\"CAM-B\",");
    const auto image=root/"review/uncalibrated-review.jpg";
    const auto report=root/"review/uncalibrated-review.json";
    const std::string base="{\"schema\":\"a0.uncalibrated-still-review.v1\",\"pairId\":\"synthetic-review-352\","
        "\"state\":\"UncalibratedReviewReady\",\"productQualityAccepted\":false,\"physicalScaleVerified\":false,"
        "\"liveToStillFieldOfViewVerified\":false,\"automaticRetryCount\":0,\"inputs\":["+input_a+","+input_b+"],"
        "\"previewGeometry\":{\"width\":640,\"height\":424,\"overlap\":320,\"vertical\":0,\"revision\":1},"
        "\"sourceDimensions\":{\"width\":7360,\"height\":4912},\"translation\":{\"x\":3680,\"y\":0},"
        "\"output\":{\"path\":\"uncalibrated-review.jpg\",\"width\":11040,\"height\":4912,\"bytes\":"+
        std::to_string(fs::file_size(image))+",\"sha256\":\""+Sha(image)+"\"}}";
    WriteText(report,base);
    const auto original_a=Sha(a),original_b=Sha(b),original_report=Sha(report),original_image=Sha(image);
    const auto original_profile=Sha(root/"draft.json"),original_job=Sha(root/"job.json"),original_color=Sha(root/"color-job.json");
    for(unsigned version=1;version<=3;++version) {
        auto selected=report;
        if(version>1) {
            const auto directory=root/("revision-"+std::to_string(version));fs::create_directory(directory);
            fs::copy_file(image,directory/"uncalibrated-review.jpg");selected=directory/"uncalibrated-review.json";
            auto text=Replace(base,"a0.uncalibrated-still-review.v1",version==2?"a0.uncalibrated-still-review.v2":"a0.uncalibrated-still-review.v3");
            text.pop_back();
            auto link=file(report);link=Replace(link,",\"bytes\":"+std::to_string(fs::file_size(report)),"");
            text+=",\"geometryBasis\":\""+std::string(version==2?"manual-still-pixels":"manual-still-similarity")+"\",\"parentReview\":"+link;
            if(version==3)text+=",\"adjustment\":{\"anchor\":\"image-center\",\"rotationDegrees\":0,\"scale\":1},\"crop\":{\"left\":0,\"top\":0,\"right\":0,\"bottom\":0}";
            WriteText(selected,text+"}");
        }
        const auto described=Run(inspector,root,{L"--report",selected.native(),L"--evaluation-inputs"});
        Require(described.exit==0&&described.error.empty(),"headless review v1/v2/v3 validation succeeds: "+described.error);
        const auto json=a0::common::protocol_json::BasicJsonParser<Failure>(described.out).Parse();
        String(json,"schemaVersion","a0.saved-review-evaluation-inputs.v1");String(json,"pairId","synthetic-review-352");
        String(At(json,"reviewReport"),"sha256",Sha(selected));
        Require(At(json,"inputs").array.size()==2&&!At(json,"productApproved").boolean,"headless pair remains unapproved");
    }
    Require(Run(inspector,root,{L"--report",report.native(),L"--evaluation-inputs",L"extra"}).exit==1,"malformed headless call exits without a dialog");
    const auto invoke=[&](const fs::path& selected,const fs::path& output,bool color=false,const fs::path& job=fs::path{}) {
        std::vector<std::wstring> args{L"-NoProfile",L"-File",script.native(),L"-Report",selected.native(),L"-Inspector",inspector.native(),
            L"-Evaluator",eval.native(),L"-ProfileFile",(root/"draft.json").native(),L"-CorrectionJob",(job.empty()?root/"job.json":job).native(),
            L"-ProductRoot",(root/"product").native(),L"-OutputDirectory",output.native(),L"-LeftUm",L"565000",L"-TopUm",L"400000",
            L"-RightUm",L"575000",L"-BottomUm",L"408000",L"-Dpi",L"100",L"-WidthPixels",L"40",L"-HeightPixels",L"32",L"-Resampling",L"bilinear"};
        if(color)args.insert(args.end(),{L"-ColorJob",(root/"color-job.json").native()});
        return Run(pwsh,root,args);
    };
    for(bool color:{false,true}) {
        const auto output=root/(color?"color-success":"geometry-success"),direct=root/(color?"color-direct":"geometry-direct");
        auto args=Arguments(root,direct);Change(args,L"--right-um",L"575000");Change(args,L"--bottom-um",L"408000");
        Change(args,L"--width-pixels",L"40");Change(args,L"--height-pixels",L"32");
        args.insert(args.end(),{L"--correction-job",(root/"job.json").native()});
        if(color)args.insert(args.end(),{L"--color-job",(root/"color-job.json").native()});
        Require(Run(eval,root,args).exit==0,"independent direct evaluation succeeds");
        const auto result=invoke(report,output,color);
        Require(result.exit==0&&result.error.empty(),"saved-pair evaluation succeeds: "+result.error+result.out);
        const auto binding=Parse(output/"review-evaluation-binding.json");
        String(binding,"result","evaluated");String(binding,"reviewReportSha256",original_report);String(binding,"reviewImageSha256",original_image);
        Require(!At(binding,"productApproved").boolean,"binding cannot grant product approval");
        Require(Sha(output/"evaluation.jpg")==Sha(direct/"evaluation.jpg"),"bridge uses the existing renderer without changing pixels");
        const auto binding_text=Text(output/"review-evaluation-binding.json");
        Require(binding_text.find(":\\")==std::string::npos && binding_text.find(":/")==std::string::npos &&
            binding_text.find("\\\\")==std::string::npos,"binding does not publish absolute or UNC paths on any drive");
        for(const auto& artifact:At(binding,"artifacts").array) {
            const auto path=output/At(artifact,"relativePath").string;
            String(artifact,"sha256",Sha(path));Number(artifact,"encodedSizeBytes",fs::file_size(path));
        }
        const auto prior=Sha(output/"review-evaluation-binding.json");
        Require(invoke(report,output,color).exit==2&&Sha(output/"review-evaluation-binding.json")==prior,"existing output is never replaced");
    }
    WriteText(root/"reject-job.json",Replace(geometry,"\"maximumDisplacementOutputPixels\": 20","\"maximumDisplacementOutputPixels\": 0.01"));
    const auto rejected=root/"rejected";const auto refusal=invoke(report,rejected,false,root/"reject-job.json");
    Require(refusal.exit==3&&refusal.error.empty(),"numeric refusal preserves exit3: "+refusal.error+refusal.out);
    String(Parse(rejected/"review-evaluation-binding.json"),"result","correction-rejected");
    Require(!fs::exists(rejected/"evaluation.jpg")&&!fs::exists(rejected/"stitch-eval.manifest.json"),"refusal has no successful image/manifest");
    const auto color_text=Text(root/"color-job.json");
    WriteText(root/"color-job.json",Replace(color_text,"\"maximumGainDifferencePercent\": 150","\"maximumGainDifferencePercent\": 0.01"));
    const auto color_rejected=root/"color-rejected";const auto color_refusal=invoke(report,color_rejected,true);
    Require(color_refusal.exit==3&&color_refusal.error.empty(),"color refusal preserves exit3: "+color_refusal.error);
    String(Parse(color_rejected/"review-evaluation-binding.json"),"result","color-rejected");
    Require(!fs::exists(color_rejected/"evaluation.jpg")&&!fs::exists(color_rejected/"stitch-eval.manifest.json"),"color refusal cannot publish success");
    WriteText(root/"color-job.json",color_text);
    fs::create_directory(root/"damaged");fs::copy_file(image,root/"damaged/uncalibrated-review.jpg");
    const auto damaged=root/"damaged/uncalibrated-review.json";
    for(const auto& text:{Replace(base,original_a,std::string(64,'0')),Replace(base,"\"productQualityAccepted\":false","\"productQualityAccepted\":true")}) {
        WriteText(damaged,text);const auto output=root/("bad-"+std::to_string(checks));
        const auto result=invoke(damaged,output);
        Require(result.exit==2&&!fs::exists(output),"invalid review fails before evaluation/output creation");
    }
    WriteText(damaged,Replace(Text(root/"revision-2/uncalibrated-review.json"),"synthetic-review-352","wrong-pair"));
    Require(invoke(damaged,root/"wrong-pair").exit==2&&!fs::exists(root/"wrong-pair"),"parent from a different pair is rejected");
    const auto a_bytes=Bytes(a);auto modified=a_bytes;modified.push_back('x');Write(a,modified);
    Require(invoke(report,root/"modified-original").exit==2&&!fs::exists(root/"modified-original"),"modified original rejected before evaluation");
    Write(a,a_bytes);
    Require(invoke(report,root/"product/excluded").exit==2&&!fs::exists(root/"product/excluded"),"product output root exclusion remains enforced");
    WriteText(root/"invalid-job.json","{}");const auto child_failed=root/"child-failed";
    Require(invoke(report,child_failed,false,root/"invalid-job.json").exit==2&&!fs::exists(child_failed),"child parser failure is never success");
    Require(Sha(a)==original_a&&Sha(b)==original_b&&Sha(report)==original_report&&Sha(image)==original_image,"all bridge paths preserve original pair and review");
    Require(Sha(root/"draft.json")==original_profile&&Sha(root/"job.json")==original_job&&Sha(root/"color-job.json")==original_color,"all bridge paths preserve profile and correction policies");
    Require(fs::is_empty(root/"product"),"product storage remains untouched");
}
