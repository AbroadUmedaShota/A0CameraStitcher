#pragma once
#include "a0/m2/document_render.hpp"
#include "a0/m2/experimental/fixed_composite_condition.hpp"
#include "a0/m2/experimental/fixed_composite_condition_v2.hpp"
#include "a0/m2/experimental/fixed_composite_condition_v3.hpp"
#include "a0/m2/experimental/fixed_composite_selection.hpp"
#include "a0/m2/experimental/mount_orientation.hpp"
#include "a0/m2/experimental/registration_mapping.hpp"
#include "a0/m2/experimental/registration_pose.hpp"
#include "a0/m2/experimental/seam_blend.hpp"
#include "a0/m2/optical_planner.hpp"
#include "a0/m2/render.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace a0::phase0::experimental {
// Live C drawn with the fixed composite condition (#401, parent #396;
// docs/design/fixed-composite.md sections 3.4 and 7). Development only,
// display only: the picture is never saved and feeds no capture, composite,
// estimate or condition (ADR-0036 decision 1, item 5).
//
// The condition (#399) is made for full-size captures. The live view frame is
// assumed to show the whole capture scaled by sx = view_width/capture_width
// and sy = view_height/capture_height (the normalized-coordinate
// correspondence of #334). That correspondence is NOT verified (#309), and
// everything here inherits that assumption. ScaleFixedSeamCondition (#398)
// turns the capture condition into the same mapping S*H*S^-1 for the view.
//
// This part is the C of the condition v1 (a similarity taken from a saved review). The C of the
// lens-aware condition v2 (#405 W5) is PlanFixedPreviewV2 below; both are chosen by ChoosePreviewC
// and drawn by RenderFixedPreview.
//
// Scale is not corrected: the live view does not know the per-capture scale
// before the shutter (ADR-0037), so C and the JPEG differ only by the scale
// within the limit and by the resolution.
//
// Blend width and brightness window. kSeamBlendDesign (8 px blend, 32 px
// half-window, 128 rows of smoothing) is stated in full-size capture pixels.
// For C each size is scaled by the same sx (columns) or sy (rows) as the view
// and rounded to whole view pixels, with a minimum of 1 (0 for the unused edge
// margin). The same ground length is therefore blended and measured, and a 8 px
// blend that is under one view pixel becomes the hard switch of one pixel that
// it already is at that resolution. A separate set of constants for C would
// make C look different from the JPEG by construction and add a second
// unapproved value, so none is introduced.
//
// A column that fits the full-size capture can fail the fit test at view scale
// only by rounding: the scaled seam and the scaled band edges are each rounded
// to whole view pixels (ScaleFixedSeamCondition documents under 2 view pixels
// for the crop edges), and the half blend width of the capture is well under
// one view pixel. PlanFixedPreview therefore tries the scaled seam column and
// then the neighbours up to kFixedPreviewSeamFitPixels away, nearest first,
// and uses the first that CanRenderFixedSeam accepts. The shift is recorded in
// the plan and shown on screen. If none fits, C is not drawn and the reason is
// shown. The full-size JPEG path is the authority for the condition itself: it
// refuses a column that does not fit at full size (seam-outside-overlap).

// Largest view side C accepts. The same bound as RenderPreviewMerge and the
// reduction of the live frames (a resource bound, not an optical value).
inline constexpr unsigned kFixedPreviewMaximumDimension=640;
// How far PlanFixedPreview may move the scaled seam column to make it fit.
inline constexpr unsigned kFixedPreviewSeamFitPixels=2;

// Design constants stated for full-size captures, scaled to a view.
inline a0::m2::experimental::SeamBlendSettings ScaleSeamBlendSettings(
    const a0::m2::experimental::SeamBlendSettings& design,double sx,double sy){
    if(!std::isfinite(sx) || !std::isfinite(sy) || !(sx>0) || !(sy>0))throw std::invalid_argument("invalid blend scale");
    const auto scaled=[](unsigned value,double factor,long long minimum){
        const auto r=std::llround(static_cast<double>(value)*factor);
        if(r>0xffffffffLL)throw std::invalid_argument("blend setting out of range");
        return static_cast<unsigned>(std::max(minimum,r));
    };
    a0::m2::experimental::SeamBlendSettings s{scaled(design.width_pixels,sx,1),scaled(design.edge_margin_pixels,sx,0),
        scaled(design.brightness_half_width_pixels,sx,1),scaled(design.brightness_smoothing_rows,sy,1)};
    a0::m2::experimental::ValidateSeamBlendSettings(s);
    return s;
}

// The grid of the lens-aware C (condition v2): the same document region as the
// JPEG of the condition, sampled at the pitch of the JPEG divided by the scale
// of the view (docs/design/fixed-composite-lens.md 6.2). Document millimetres.
struct FixedPreviewGrid {
    double left_mm{},top_mm{},pitch_mm{};
    std::uint32_t width{},height{};
};

struct LiveRegistrationContext;

// Everything C needs besides the two frames. Pure data; no pixel is read.
// Copying it is cheap: the coordinate table of a lens-aware C is shared.
struct FixedPreviewPlan {
    // Empty: C can be drawn. Otherwise why not (the codes of the JPEG path).
    std::optional<a0::m2::experimental::FixedCompositeSkip> skip;
    std::string detail;
    unsigned view_width{},view_height{};
    double scale_x{},scale_y{};
    // The mapping and seam column in view pixels, as RenderPairFixedSeam takes them.
    // view.seam_column is the column in use for both versions; view.parameters is v1 only.
    a0::m2::experimental::FixedSeamCondition view;
    a0::m2::experimental::SeamBlendSettings settings{};
    std::uint32_t output_width{},output_height{};
    // From the condition record: the column of the full-size capture (v1) or of the
    // output raster of the JPEG (v2).
    std::uint32_t capture_seam_column{};
    a0::m2::experimental::FixedSeamOrigin seam_origin{a0::m2::experimental::FixedSeamOrigin::band_center_provisional};
    // The seam column scaled to C before the fit, and the signed distance the fit moved it.
    std::uint32_t scaled_seam_column{};
    int seam_shift{};
    // Condition v2 only. lens_aware: C is drawn through the output-to-live-view mappings of the
    // calibrated lens. mapped is the pair of coordinate tables (output_width x output_height).
    bool lens_aware{};
    FixedPreviewGrid grid;
    std::shared_ptr<const a0::m2::experimental::MappedFixedSeamInput> mapped;
    std::optional<std::string> profile_id;
    // Condition v3 only (#417 LC): what the registered C needs to bake camera B's table again. Null for v1 and v2.
    std::shared_ptr<const LiveRegistrationContext> live;
};

// Scales the condition to a view of view_width x view_height (the oriented,
// reduced live frames, equal for A and B). `orientation` is the mount
// orientation of the running C; a condition made for the other one is refused.
// Never throws: a refusal is in plan.skip.
inline FixedPreviewPlan PlanFixedPreview(const a0::m2::experimental::FixedCompositeCondition& condition,
    a0::m2::experimental::MountOrientation orientation,unsigned view_width,unsigned view_height){
    using a0::m2::experimental::FixedCompositeSkip;
    FixedPreviewPlan plan;
    plan.view_width=view_width;plan.view_height=view_height;
    plan.capture_seam_column=condition.seam.column;plan.seam_origin=condition.seam.origin;
    const auto refuse=[&](FixedCompositeSkip skip,std::string detail){plan.skip=skip;plan.detail=std::move(detail);return plan;};
    if(condition.orientation!=orientation)
        return refuse(FixedCompositeSkip::orientation_differs,std::string("condition ")+a0::m2::experimental::MountOrientationName(condition.orientation)+", live "+a0::m2::experimental::MountOrientationName(orientation));
    if(!view_width || !view_height || view_width>kFixedPreviewMaximumDimension || view_height>kFixedPreviewMaximumDimension)
        return refuse(FixedCompositeSkip::size_differs,"view "+std::to_string(view_width)+"x"+std::to_string(view_height));
    a0::m2::experimental::FixedSeamCondition capture;
    try{capture={a0::m2::experimental::FixedCompositeRenderParameters(condition),condition.seam.column};}
    catch(const std::invalid_argument& error){return refuse(FixedCompositeSkip::condition_invalid,error.what());}
    a0::m2::experimental::FixedSeamCondition scaled;
    try{scaled=a0::m2::experimental::ScaleFixedSeamCondition(capture,condition.source_width,condition.source_height,view_width,view_height);}
    catch(const std::invalid_argument& error){
        // The seam alone is the cause when the same mapping scales with the seam on the first column
        // (the rounded seam fell off the scaled output); otherwise the size cannot be scaled to.
        try{(void)a0::m2::experimental::ScaleFixedSeamCondition({capture.parameters,0},condition.source_width,condition.source_height,view_width,view_height);}
        catch(const std::invalid_argument&){return refuse(FixedCompositeSkip::size_differs,error.what());}
        return refuse(FixedCompositeSkip::seam_outside_overlap,error.what());
    }
    plan.scale_x=static_cast<double>(view_width)/condition.source_width;plan.scale_y=static_cast<double>(view_height)/condition.source_height;
    try{plan.settings=ScaleSeamBlendSettings(a0::m2::experimental::kSeamBlendDesign,plan.scale_x,plan.scale_y);}
    catch(const std::invalid_argument& error){return refuse(FixedCompositeSkip::size_differs,error.what());}
    const auto canvas=a0::m2::render::PlanPairCanvas(view_width,view_height,view_width,view_height,scaled.parameters);
    plan.output_width=canvas.output_width;plan.output_height=canvas.output_height;
    plan.scaled_seam_column=scaled.seam_column;
    // Coverage depends on the geometry only, so blank frames decide the fit.
    const a0::m2::render::BgrImage blank{view_width,view_height,std::vector<std::uint8_t>(static_cast<size_t>(view_width)*view_height*3)};
    std::vector<int> offsets{0};
    for(int distance=1;distance<=static_cast<int>(kFixedPreviewSeamFitPixels);++distance){offsets.push_back(-distance);offsets.push_back(distance);}
    for(const int offset:offsets){
        const long long column=static_cast<long long>(scaled.seam_column)+offset;
        if(column<0 || column>=static_cast<long long>(plan.output_width))continue;
        bool fits=false;
        try{fits=a0::m2::experimental::CanRenderFixedSeam(blank,blank,scaled.parameters,static_cast<std::uint32_t>(column),plan.settings);}
        catch(const std::invalid_argument&){fits=false;} // a row without A and B: nothing fits
        if(fits){
            plan.view={scaled.parameters,static_cast<std::uint32_t>(column)};plan.seam_shift=offset;
            return plan;
        }
    }
    return refuse(FixedCompositeSkip::seam_outside_overlap,"scaled seam column "+std::to_string(scaled.seam_column)+" of "+std::to_string(plan.output_width));
}

// ---- The lens-aware C (condition v2, #405 W5; docs/design/fixed-composite-lens.md 6) ----
//
// The condition v2 names a calibrated draft rig profile and an output raster of the document
// plane. C is drawn through the same mappings as the JPEG, with two more steps after the lens:
//
//   output pixel (i,j) -> document point (left+(i+0.5)p_C, top+(j+0.5)p_C) mm
//     -> raw pixel of the full-size capture (the lens coefficients, render::TryProjectDocument)
//     -> pixel of the live view stored order: S(u,v) = (sx*u, sy*v)
//     -> pixel of the oriented live view: (vw-1-u, vh-1-v) under rotated-180, else unchanged
//
// S is the same assumption as the v1 C (#401): the live view frame shows the whole capture scaled
// by sx = view_width/7360 and sy = view_height/4912, with no offset. It is NOT verified (#309).
// The orientation step is only the swap of pixels that OrientRenderInput makes on the live frames
// before C is drawn; it is a coordinate flip here and adds no resampling (design 3.7). The only
// sampling is the one bilinear read of the oriented live frame inside the blend.
//
// The grid of C. p_C = p/s with p = 25.4/dpi of the condition's raster and s = sx. Its columns and
// rows are the number of whole p_C that fit in the region (rounded down), so that every sample lies
// inside the region that the JPEG was checked over. Scale is not corrected (k = 1).
//
// The seam. The JPEG column c has its centre on the document X of C's column (c+0.5)*s-0.5. That
// is rounded and, as in the v1 C, moved by at most kFixedPreviewSeamFitPixels to the nearest column
// that CanRenderMappedFixedSeam accepts. The blend width and the brightness window are scaled as in
// the v1 C (ScaleSeamBlendSettings) with s for columns and rows alike, because the grid is square
// on the document.
//
// The coordinate table. The mappings do not change during a preview session, so the position in
// the live frame of every output pixel is computed once, when the plan is made, and a frame only
// reads it. The table holds coordinates, not an image; the live frame is still read once, by one
// bilinear sampling. It is bit for bit what the direct mapping returns (LiveViewOutputToCamera).

// Largest number of samples of the lens-aware C (a resource bound for the table, not an optical value).
inline constexpr std::uint64_t kFixedPreviewMaximumOutputPixels=2'000'000;

// Throws std::invalid_argument for a raster that is not exact, a scale that is not finite and
// positive, a region that holds no sample, or more than kFixedPreviewMaximumOutputPixels samples.
inline FixedPreviewGrid MakeFixedPreviewGrid(const a0::m2::render::OutputRaster& raster,double scale){
    a0::m2::render::ValidateOutputRaster(raster);
    if(!std::isfinite(scale) || !(scale>0))throw std::invalid_argument("invalid preview scale");
    const auto& r=raster.region_um;
    FixedPreviewGrid grid;
    grid.pitch_mm=25.4/static_cast<double>(raster.dpi)/scale;
    grid.left_mm=static_cast<double>(r.left)/1000.0;grid.top_mm=static_cast<double>(r.top)/1000.0;
    const double columns=std::floor(static_cast<double>(r.right-r.left)/1000.0/grid.pitch_mm);
    const double rows=std::floor(static_cast<double>(r.bottom-r.top)/1000.0/grid.pitch_mm);
    if(!(columns>=1) || !(rows>=1))throw std::invalid_argument("the region holds no sample of the preview grid");
    if(columns*rows>static_cast<double>(kFixedPreviewMaximumOutputPixels))throw std::invalid_argument("the preview grid has too many samples");
    grid.width=static_cast<std::uint32_t>(columns);grid.height=static_cast<std::uint32_t>(rows);
    return grid;
}

// The mapping of one camera for the grid: output pixel (x,y), passed as its integer index, to the
// pixel of the oriented live frame. camera is the calibrated projection of the profile (a copy is
// held). Throws std::invalid_argument, when called, where the projection is not representable.
// sx, sy: live view pixels per capture pixel. turned: the live frame was rotated by 180 degrees.
inline a0::m2::render::OutputToCamera LiveViewOutputToCamera(const a0::m2::render::CameraProjection& camera,const FixedPreviewGrid& grid,
    double sx,double sy,unsigned view_width,unsigned view_height,bool turned){
    if(!std::isfinite(sx) || !std::isfinite(sy) || !(sx>0) || !(sy>0) || !view_width || !view_height)throw std::invalid_argument("invalid live view scale");
    const double right=static_cast<double>(view_width)-1,bottom=static_cast<double>(view_height)-1;
    return [camera,grid,sx,sy,right,bottom,turned](a0::m2::render::Point output,a0::m2::render::Point& live){
        const a0::m2::render::Point document{grid.left_mm+(output.x+.5)*grid.pitch_mm,grid.top_mm+(output.y+.5)*grid.pitch_mm};
        a0::m2::render::Point raw{};
        if(!a0::m2::render::TryProjectDocument(camera,document,raw))throw std::invalid_argument("document projection is not numerically representable");
        live={raw.x*sx,raw.y*sy};
        if(turned)live={right-live.x,bottom-live.y};
        return true;
    };
}

// A mapping evaluated once for every output pixel and kept: reading it returns what the mapping
// returned, bit for bit. Throws what the mapping throws, and for a non-finite coordinate.
inline std::shared_ptr<const std::vector<a0::m2::render::Point>> BakeOutputToCamera(const a0::m2::render::OutputToCamera& map,
    std::uint32_t width,std::uint32_t height){
    if(!map || !width || !height)throw std::invalid_argument("nothing to bake");
    auto points=std::make_shared<std::vector<a0::m2::render::Point>>();
    points->resize(static_cast<size_t>(width)*height);
    constexpr double kAbsent=std::numeric_limits<double>::quiet_NaN();
    for(std::uint32_t y=0;y<height;++y)for(std::uint32_t x=0;x<width;++x){
        a0::m2::render::Point p{};
        if(!map({static_cast<double>(x),static_cast<double>(y)},p)){p={kAbsent,kAbsent};}
        else if(!std::isfinite(p.x) || !std::isfinite(p.y))throw std::invalid_argument("output-to-camera mapping returned nonfinite coordinates");
        (*points)[static_cast<size_t>(y)*width+x]=p;
    }
    return points;
}
inline a0::m2::render::OutputToCamera TableOutputToCamera(std::shared_ptr<const std::vector<a0::m2::render::Point>> points,
    std::uint32_t width,std::uint32_t height){
    if(!points || points->size()!=static_cast<size_t>(width)*height)throw std::invalid_argument("coordinate table does not match its size");
    return [points=std::move(points),width,height](a0::m2::render::Point output,a0::m2::render::Point& raw){
        if(!(output.x>=0) || !(output.y>=0) || !(output.x<static_cast<double>(width)) || !(output.y<static_cast<double>(height)))return false;
        raw=(*points)[static_cast<size_t>(output.y)*width+static_cast<size_t>(output.x)];
        return !std::isnan(raw.x);
    };
}

// The pair of direct mappings of the grid (A and B), not baked. For a plan this is what the table holds.
inline a0::m2::experimental::MappedFixedSeamInput LiveViewMappedInput(const a0::m2::render::DocumentRenderParameters& profile,
    const FixedPreviewGrid& grid,double sx,double sy,unsigned view_width,unsigned view_height,bool turned){
    return {grid.width,grid.height,
        LiveViewOutputToCamera(profile.camera_a,grid,sx,sy,view_width,view_height,turned),
        LiveViewOutputToCamera(profile.camera_b,grid,sx,sy,view_width,view_height,turned)};
}

// What the lens-aware planner needs of a condition record: the profile's cameras, the output raster, the seam and the mount
// orientation. Pointers into a loaded record, which the caller keeps alive for the call.
struct LensPreviewCondition {
    const a0::m2::render::DocumentRenderParameters* parameters{};
    const a0::m2::render::OutputRaster* raster{};
    a0::m2::experimental::MountOrientation orientation{a0::m2::experimental::MountOrientation::normal};
    std::uint32_t seam_column{};
    a0::m2::experimental::FixedSeamOrigin seam_origin{a0::m2::experimental::FixedSeamOrigin::band_center_provisional};
    std::optional<std::string> profile_id;
};
// Camera B's mapping on C's grid, for a C whose B is not B's calibrated mapping. Empty: the calibrated mapping.
using PreviewBMapper=std::function<a0::m2::render::OutputToCamera(const FixedPreviewGrid& grid,double sx,double sy,
    unsigned view_width,unsigned view_height,bool turned)>;
// The two coordinate tables of a plan, for a caller that bakes B again (the table of A never changes in a session).
struct PreviewTables {
    std::shared_ptr<const std::vector<a0::m2::render::Point>> camera_a,camera_b;
};

// Plans the lens-aware C of a condition record for a view of view_width x view_height (the oriented, reduced live frames, equal
// for A and B). `orientation` is the mount orientation of the running C; a condition made for the other one is refused. Never
// throws for a condition: a refusal is in plan.skip, and C is drawn only if a trial render over the whole grid succeeds.
// The condition v2 (PlanFixedPreviewV2) and the condition v3 (PlanFixedPreviewV3) share this body; map_b and tables are the
// only differences.
inline FixedPreviewPlan PlanLensAwarePreview(const LensPreviewCondition& condition,a0::m2::experimental::MountOrientation orientation,
    unsigned view_width,unsigned view_height,const PreviewBMapper& map_b,PreviewTables* tables){
    namespace m2=a0::m2::experimental;
    using m2::FixedCompositeSkip;
    FixedPreviewPlan plan;
    plan.lens_aware=true;plan.view_width=view_width;plan.view_height=view_height;
    plan.capture_seam_column=condition.seam_column;plan.seam_origin=condition.seam_origin;plan.profile_id=condition.profile_id;
    // A refused plan holds no table.
    const auto refuse=[&](FixedCompositeSkip skip,std::string detail){plan.skip=skip;plan.detail=std::move(detail);plan.mapped.reset();return plan;};
    if(condition.orientation!=orientation)
        return refuse(FixedCompositeSkip::orientation_differs,std::string("condition ")+m2::MountOrientationName(condition.orientation)+", live "+m2::MountOrientationName(orientation));
    if(!view_width || !view_height || view_width>kFixedPreviewMaximumDimension || view_height>kFixedPreviewMaximumDimension)
        return refuse(FixedCompositeSkip::size_differs,"view "+std::to_string(view_width)+"x"+std::to_string(view_height));
    const double sx=static_cast<double>(view_width)/static_cast<double>(a0::m2::kD810SensorWidthPixels);
    const double sy=static_cast<double>(view_height)/static_cast<double>(a0::m2::kD810SensorHeightPixels);
    plan.scale_x=sx;plan.scale_y=sy;
    try{plan.settings=ScaleSeamBlendSettings(m2::kSeamBlendDesign,sx,sx);plan.grid=MakeFixedPreviewGrid(*condition.raster,sx);}
    catch(const std::invalid_argument& error){return refuse(FixedCompositeSkip::size_differs,error.what());}
    plan.output_width=plan.grid.width;plan.output_height=plan.grid.height;
    const bool turned=orientation==m2::MountOrientation::rotated_180;
    try{
        const auto& parameters=*condition.parameters;
        const auto direct_a=LiveViewOutputToCamera(parameters.camera_a,plan.grid,sx,sy,view_width,view_height,turned);
        const auto direct_b=map_b?map_b(plan.grid,sx,sy,view_width,view_height,turned)
            :LiveViewOutputToCamera(parameters.camera_b,plan.grid,sx,sy,view_width,view_height,turned);
        const auto table_a=BakeOutputToCamera(direct_a,plan.output_width,plan.output_height);
        const auto table_b=BakeOutputToCamera(direct_b,plan.output_width,plan.output_height);
        plan.mapped=std::make_shared<const m2::MappedFixedSeamInput>(m2::MappedFixedSeamInput{plan.output_width,plan.output_height,
            TableOutputToCamera(table_a,plan.output_width,plan.output_height),TableOutputToCamera(table_b,plan.output_width,plan.output_height)});
        if(tables){tables->camera_a=table_a;tables->camera_b=table_b;}
    }catch(const std::exception& error){return refuse(FixedCompositeSkip::condition_invalid,error.what());}
    // Coverage depends on the geometry only, so flat frames decide the fit and the trial render.
    const a0::m2::render::BgrImage flat{view_width,view_height,std::vector<std::uint8_t>(static_cast<size_t>(view_width)*view_height*3,128)};
    // An output pixel that neither camera shows (the condition, edited by hand, asks for more than the cameras see) is a size
    // problem, as in the JPEG path; without this a row with nothing in it would read as a seam outside the overlap.
    try{
        for(std::uint32_t j=0;j<plan.output_height;++j)for(std::uint32_t i=0;i<plan.output_width;++i){
            const a0::m2::render::Point output{static_cast<double>(i),static_cast<double>(j)};
            a0::m2::render::Point raw_a{},raw_b{};
            const auto inside=[&](const a0::m2::render::Point& p){return p.x>=0 && p.y>=0 && p.x<view_width && p.y<view_height;};
            const bool a_shows=plan.mapped->camera_a(output,raw_a) && inside(raw_a);
            const bool b_shows=plan.mapped->camera_b(output,raw_b) && inside(raw_b);
            if(!a_shows && !b_shows)return refuse(FixedCompositeSkip::size_differs,"output pixel ("+std::to_string(i)+","+std::to_string(j)+") is shown by neither camera");
        }
    }catch(const std::invalid_argument& error){return refuse(FixedCompositeSkip::size_differs,error.what());}
    // The JPEG column c is centred on the document X of C's column (c+0.5)*s-0.5; rounded half up.
    const long long target=static_cast<long long>(std::floor((static_cast<double>(condition.seam_column)+0.5)*sx));
    plan.scaled_seam_column=static_cast<std::uint32_t>(std::clamp<long long>(target,0,static_cast<long long>(plan.output_width)-1));
    std::vector<int> offsets{0};
    for(int distance=1;distance<=static_cast<int>(kFixedPreviewSeamFitPixels);++distance){offsets.push_back(-distance);offsets.push_back(distance);}
    for(const int offset:offsets){
        const long long column=target+offset;
        if(column<0 || column>=static_cast<long long>(plan.output_width))continue;
        bool fits=false;
        try{fits=m2::CanRenderMappedFixedSeam(flat,flat,*plan.mapped,static_cast<std::uint32_t>(column),plan.settings);}
        catch(const std::invalid_argument&){fits=false;} // a row without A and B: nothing fits
        if(!fits)continue;
        try{(void)m2::RenderMappedFixedSeam(flat,flat,*plan.mapped,static_cast<std::uint32_t>(column),plan.settings);}
        catch(const std::invalid_argument& error){ // a sample covered by neither camera at this size
            return refuse(FixedCompositeSkip::size_differs,error.what());
        }
        plan.view.seam_column=static_cast<std::uint32_t>(column);plan.seam_shift=offset;
        return plan;
    }
    return refuse(FixedCompositeSkip::seam_outside_overlap,"scaled seam column "+std::to_string(target)+" of "+std::to_string(plan.output_width));
}

// Plans the lens-aware C of a loaded condition v2 (see PlanLensAwarePreview).
inline FixedPreviewPlan PlanFixedPreviewV2(const a0::m2::experimental::LoadedFixedCompositeConditionV2& loaded,
    a0::m2::experimental::MountOrientation orientation,unsigned view_width,unsigned view_height){
    const auto& condition=loaded.condition;
    const LensPreviewCondition source{&loaded.profile.parameters,&condition.raster,condition.orientation,condition.seam.column,condition.seam.origin,
        loaded.profile.profile_id};
    return PlanLensAwarePreview(source,orientation,view_width,view_height,{},nullptr);
}

// ---- The registered C (condition v3, #417 LC; docs/design/fixed-composite-registration.md 4 "the live C", addendum 11.21) ----
//
// C draws camera B through the same mapping as the JPEG's layer 1 (G(p) Pi(pi), registration_pose.hpp): the pose p and the paper
// surface pi. Layer 2 is not drawn, and nothing that C estimates goes to the JPEG. The mapping is the one of LiveViewOutputToCamera
// with one more step on B's document point:
//
//   output pixel (x,y) of C's grid -> document point (left+(x+0.5)p_C, top+(y+0.5)p_C) mm      (the grid of the v2 C)
//     -> G(p) Pi(pi) of that point                                                              (B only; A is the calibrated mapping)
//     -> raw pixel of the full-size capture (the lens coefficients, render::TryProjectDocument)
//     -> pixel of the live view, stored order: (sx*u, sy*v), raw*s without a half-pixel shift
//     -> pixel of the oriented live view: (vw-1-u, vh-1-v) under rotated-180, else unchanged
//
// The mount orientation is the flip of the last step and is not applied to p or pi: the pose is B's in the stored (raw) frame, so
// the same physical movement gives the same pose for either mounting. The surface origin (x0, y0) of Pi is the JPEG's: the
// middle of the condition's seam column on the condition's raster (SurfaceOriginMm), not a point of C's grid, so that pi means the
// same thing in C and in the JPEG.
//
// With p = p_b and pi = 0 (the condition's fixed mapping) and a zero base pose the table of B is the v2 C's table bit for bit,
// because the mapping is then LiveViewOutputToCamera itself. A pose without a surface is PoseModel::Homography(p), the maker of
// fixed_composite_maps.hpp (PoseCorrectedDocumentOutputToCamera); with a surface it is RegisteredHomography with the same
// checks (the surface is below both cameras at the corners of the raster widened by one pixel), so what is drawn is what the
// maker gives for the same mapping on the same raster.

struct LiveRegistrationContext {
    a0::m2::render::CameraProjection camera_b;
    a0::m2::experimental::content_registration::PoseModel model_b;
    a0::m2::experimental::content_registration::CameraCentre centre_a;
    a0::m2::render::Point origin_mm;                 // the surface origin (x0, y0) of the condition's raster and seam column
    a0::m2::render::OutputRaster raster;             // the condition's (the JPEG's) raster, not C's grid
    std::uint32_t seam_column;                       // the condition's column
    FixedPreviewGrid grid;
    double sx,sy;
    unsigned view_width,view_height;
    bool turned;
    a0::m2::experimental::RegisteredMapping fixed;   // {p_b, no surface}: the condition's fixed mapping, G4
    // registration.live.limits of the condition, in C's pixels. Absent: C is drawn with the fixed mapping only.
    std::optional<a0::m2::experimental::content_registration::RegistrationLimits> live_limits;
    // The tables in force: A's (made once for the session) and B's (made for `baked`).
    std::shared_ptr<const std::vector<a0::m2::render::Point>> table_a,table_b;
    a0::m2::experimental::RegisteredMapping baked;
    LiveRegistrationContext(const a0::m2::render::DocumentRenderParameters& profile,const a0::m2::render::OutputRaster& condition_raster,
        std::uint32_t condition_seam_column,const FixedPreviewGrid& preview_grid,double scale_x,double scale_y,unsigned width,unsigned height,
        bool flipped,const a0::m2::experimental::RegisteredMapping& fixed_mapping)
        :camera_b(profile.camera_b),model_b(profile.camera_b),
         centre_a(a0::m2::experimental::content_registration::PoseModel(profile.camera_a).Centre({})),
         origin_mm(a0::m2::experimental::content_registration::SurfaceOriginMm(condition_raster,condition_seam_column)),
         raster(condition_raster),seam_column(condition_seam_column),grid(preview_grid),sx(scale_x),sy(scale_y),view_width(width),
         view_height(height),turned(flipped),fixed(fixed_mapping),baked(fixed_mapping){}
};

// The document homography of a mapping (document mm to document mm), or nullopt for the identity (no pose and no surface). Throws
// std::invalid_argument for layer 2 (C does not draw it), a pose or surface that is not finite, and a surface that is not below
// both cameras over the raster.
inline std::optional<a0::m2::render::Matrix3> LiveDocumentHomography(const LiveRegistrationContext& context,
    const a0::m2::experimental::RegisteredMapping& mapping){
    namespace cr=a0::m2::experimental::content_registration;
    if(mapping.parallax)throw std::invalid_argument("layer 2 is not drawn by the live C");
    const auto& p=mapping.pose;
    for(const double v:{p.rx_deg,p.ry_deg,p.rz_deg,p.tx_mm,p.ty_mm,p.tz_percent})if(!std::isfinite(v))throw std::invalid_argument("a pose must be finite");
    const auto& s=mapping.surface;
    if(!std::isfinite(s.h0_mm) || !std::isfinite(s.slope_x) || !std::isfinite(s.slope_y))throw std::invalid_argument("the surface plane must be finite");
    if(p.IsZero() && s.IsZero())return std::nullopt;
    a0::m2::render::Matrix3 h{};
    if(s.IsZero())h=context.model_b.Homography(p);
    else{
        const auto centre_b=context.model_b.Centre(p);
        const auto frame=cr::FrameOfRaster(context.raster);
        constexpr double kReach=1.0; // the same widening as the maker's, with no layer 2
        for(const double ix:{-kReach,static_cast<double>(context.raster.width_pixels)+kReach})
            for(const double iy:{-kReach,static_cast<double>(context.raster.height_pixels)+kReach})
                if(!cr::SurfaceHomographyHoldsAt(context.centre_a,centre_b,context.origin_mm,s,{frame.left_mm+ix*frame.pitch_mm,frame.top_mm+iy*frame.pitch_mm}))
                    throw std::invalid_argument("the surface is not below both cameras over the raster");
        h=cr::RegisteredHomography(context.model_b,context.centre_a,context.origin_mm,p,s);
    }
    for(const double v:h)if(!std::isfinite(v))throw std::invalid_argument("the document homography of a registered mapping must be finite");
    return h;
}

// The mapping of camera B on C's grid for `mapping`: output pixel index (x,y) to the pixel of the oriented live frame. Throws
// what LiveDocumentHomography throws and, when called, where the projection is not representable. A mapping with neither a pose
// nor a surface is LiveViewOutputToCamera(camera_b, ...) itself, so it is the v2 C's mapping bit for bit.
inline a0::m2::render::OutputToCamera LiveViewRegisteredOutputToCamera(const LiveRegistrationContext& context,
    const a0::m2::experimental::RegisteredMapping& mapping){
    const auto h=LiveDocumentHomography(context,mapping);
    if(!h)return LiveViewOutputToCamera(context.camera_b,context.grid,context.sx,context.sy,context.view_width,context.view_height,context.turned);
    const double right=static_cast<double>(context.view_width)-1,bottom=static_cast<double>(context.view_height)-1;
    return [camera=context.camera_b,grid=context.grid,sx=context.sx,sy=context.sy,right,bottom,turned=context.turned,g=*h](
        a0::m2::render::Point output,a0::m2::render::Point& live){
        const a0::m2::render::Point document{grid.left_mm+(output.x+.5)*grid.pitch_mm,grid.top_mm+(output.y+.5)*grid.pitch_mm};
        a0::m2::render::Point moved{},raw{};
        if(!a0::m2::render::TryTransform(g,document,moved))return false;
        if(!a0::m2::render::TryProjectDocument(camera,moved,raw))throw std::invalid_argument("document projection is not numerically representable");
        live={raw.x*sx,raw.y*sy};
        if(turned)live={right-live.x,bottom-live.y};
        return true;
    };
}

// Plans the C of a loaded condition v3: the fixed mapping {p_b, no surface} is drawn until a registration is baked in
// (RebakePreviewB). Refusals are the v2 C's. plan.live is set for a plan that is drawn. registration.live (the limits of the live
// estimate, in C's pixels) being null means C is never registered: LiveRegistrationDisabledReason names it.
inline FixedPreviewPlan PlanFixedPreviewV3(const a0::m2::experimental::LoadedFixedCompositeConditionV3& loaded,
    a0::m2::experimental::MountOrientation orientation,unsigned view_width,unsigned view_height){
    const auto& condition=loaded.condition;
    const LensPreviewCondition source{&loaded.profile.parameters,&condition.raster,condition.orientation,condition.seam.column,condition.seam.origin,
        loaded.profile.profile_id};
    const a0::m2::experimental::RegisteredMapping fixed{condition.base_pose,{},std::nullopt};
    std::shared_ptr<LiveRegistrationContext> context;
    PreviewTables tables;
    auto plan=PlanLensAwarePreview(source,orientation,view_width,view_height,
        [&](const FixedPreviewGrid& grid,double sx,double sy,unsigned width,unsigned height,bool turned){
            context=std::make_shared<LiveRegistrationContext>(loaded.profile.parameters,condition.raster,condition.seam.column,grid,sx,sy,width,height,turned,fixed);
            return LiveViewRegisteredOutputToCamera(*context,fixed);
        },&tables);
    if(plan.skip || !context)return plan;
    context->table_a=tables.camera_a;context->table_b=tables.camera_b;
    if(condition.registration.live)context->live_limits=condition.registration.live->limits;
    plan.live=std::move(context);
    return plan;
}

// "live-registration-disabled" for a v3 plan whose condition has no registration.live; empty for any other plan.
inline std::string LiveRegistrationDisabledReason(const FixedPreviewPlan& plan){
    if(plan.live && !plan.live->live_limits)return "live-registration-disabled";
    return {};
}

struct LiveRebakeResult {
    bool ok{false};
    FixedPreviewPlan plan;     // ok: the plan with B's new table. Not ok: the plan that was given, unchanged
    std::string detail;        // not ok: why (English, for logs)
};
// Bakes camera B's table again for `mapping` and returns a new plan that shares everything else with `plan`, including the table
// of A (A's mapping never changes in a session). The plan that was given is not changed, so a frame that is being drawn from it
// is not disturbed: the caller switches to the returned plan between frames (two faces). The work is one pass over C's grid, which
// is bounded by kFixedPreviewMaximumOutputPixels. A mapping that cannot be drawn (not representable, a sample that neither
// camera shows, the seam no longer inside the overlap, layer 2, a surface above a camera) is not ok and changes nothing.
inline LiveRebakeResult RebakePreviewB(const FixedPreviewPlan& plan,const a0::m2::experimental::RegisteredMapping& mapping){
    namespace m2=a0::m2::experimental;
    LiveRebakeResult result;
    result.plan=plan;
    if(plan.skip || !plan.live || !plan.mapped){result.detail="no registered plan";return result;}
    try{
        const auto& context=*plan.live;
        const auto direct=LiveViewRegisteredOutputToCamera(context,mapping);
        const auto table_b=BakeOutputToCamera(direct,plan.output_width,plan.output_height);
        auto mapped=std::make_shared<const m2::MappedFixedSeamInput>(m2::MappedFixedSeamInput{plan.output_width,plan.output_height,
            plan.mapped->camera_a,TableOutputToCamera(table_b,plan.output_width,plan.output_height)});
        // The same checks as the plan's, on the seam column in force: every sample is shown by a camera, the seam and half the
        // blend width lie in the overlap, and a trial render over the whole grid succeeds.
        const auto inside=[&](const a0::m2::render::Point& p){return p.x>=0 && p.y>=0 && p.x<plan.view_width && p.y<plan.view_height;};
        for(std::uint32_t j=0;j<plan.output_height;++j)for(std::uint32_t i=0;i<plan.output_width;++i){
            const a0::m2::render::Point output{static_cast<double>(i),static_cast<double>(j)};
            a0::m2::render::Point raw_a{},raw_b{};
            const bool a_shows=mapped->camera_a(output,raw_a) && inside(raw_a);
            const bool b_shows=mapped->camera_b(output,raw_b) && inside(raw_b);
            if(!a_shows && !b_shows)throw std::invalid_argument("output pixel ("+std::to_string(i)+","+std::to_string(j)+") is shown by neither camera");
        }
        const a0::m2::render::BgrImage flat{plan.view_width,plan.view_height,
            std::vector<std::uint8_t>(static_cast<size_t>(plan.view_width)*plan.view_height*3,128)};
        if(!m2::CanRenderMappedFixedSeam(flat,flat,*mapped,plan.view.seam_column,plan.settings))throw std::invalid_argument("the seam column is no longer inside the overlap");
        (void)m2::RenderMappedFixedSeam(flat,flat,*mapped,plan.view.seam_column,plan.settings);
        auto next=std::make_shared<LiveRegistrationContext>(context);
        next->table_b=table_b;next->baked=mapping;
        result.plan.mapped=std::move(mapped);result.plan.live=std::move(next);result.ok=true;
    }catch(const std::exception& error){result.detail=error.what();}
    return result;
}

// The largest distance, in C's output pixels, between where `a` and `b` put the same output pixel of C in the document, over a
// sparse lattice of samples_per_axis x samples_per_axis pixels of C's grid (corners included) that camera B shows. This is the
// "difference of two mappings" that decides whether B's table is baked again (LiveRegistrationTracker::DecideRebake). It is
// measured on the document plane, so it does not depend on the lens or the size of the view. +infinity where a mapping cannot
// place a sample. Throws std::invalid_argument for a plan without a registered context, fewer than 2 samples per axis and what
// LiveDocumentHomography throws.
inline double LiveMappingDifferencePx(const FixedPreviewPlan& plan,const a0::m2::experimental::RegisteredMapping& a,
    const a0::m2::experimental::RegisteredMapping& b,unsigned samples_per_axis){
    if(!plan.live || !plan.live->table_b)throw std::invalid_argument("no registered plan");
    if(samples_per_axis<2)throw std::invalid_argument("at least 2 samples per axis");
    const auto& context=*plan.live;
    const auto ha=LiveDocumentHomography(context,a),hb=LiveDocumentHomography(context,b);
    const auto& grid=context.grid;
    const auto index=[&](unsigned k,std::uint32_t size){
        return static_cast<std::uint32_t>(std::llround(static_cast<double>(k)*static_cast<double>(size-1)/static_cast<double>(samples_per_axis-1)));
    };
    double largest=0;
    for(unsigned jy=0;jy<samples_per_axis;++jy)for(unsigned jx=0;jx<samples_per_axis;++jx){
        const auto x=index(jx,grid.width),y=index(jy,grid.height);
        if(std::isnan((*context.table_b)[static_cast<size_t>(y)*grid.width+x].x))continue; // B does not show this sample
        const a0::m2::render::Point document{grid.left_mm+(x+.5)*grid.pitch_mm,grid.top_mm+(y+.5)*grid.pitch_mm};
        a0::m2::render::Point pa=document,pb=document;
        if((ha && !a0::m2::render::TryTransform(*ha,document,pa)) || (hb && !a0::m2::render::TryTransform(*hb,document,pb)))
            return std::numeric_limits<double>::infinity();
        largest=std::max(largest,std::hypot(pa.x-pb.x,pa.y-pb.y)/grid.pitch_mm);
    }
    return largest;
}

// One frame pair of C. A and B are the oriented, reduced live frames of the
// size the plan was made for. The brightness ratio is measured here, in every
// call, from these two frames; the mapping and the seam column never move.
inline a0::m2::experimental::SeamBlendResult RenderFixedPreview(const FixedPreviewPlan& plan,
    const a0::m2::render::BgrImage& a,const a0::m2::render::BgrImage& b){
    if(plan.skip)throw std::invalid_argument("fixed composite preview has no plan");
    if(a.width!=plan.view_width || a.height!=plan.view_height || b.width!=plan.view_width || b.height!=plan.view_height)
        throw std::invalid_argument("frames differ from the planned view size");
    if(plan.lens_aware){
        if(!plan.mapped)throw std::invalid_argument("fixed composite preview has no mapping");
        return a0::m2::experimental::RenderMappedFixedSeam(a,b,*plan.mapped,plan.view.seam_column,plan.settings);
    }
    return a0::m2::experimental::RenderPairFixedSeam(a,b,plan.view.parameters,plan.view.seam_column,plan.settings);
}

// The condition file beside the settings, read when a preview session starts.
// The preview's view of the condition file, of any version: loaded holds a version 1
// record, loaded_v2 a version 2 record with its verified profile, loaded_v3 a version 3 record (#417)
// with its verified profile. At most one is set.
// When neither is, reason says why: condition_absent (no file) leaves the uncalibrated C;
// anything else means a file exists and C is not drawn with it. The reading itself is
// SelectFixedComposite, the function the pair capture uses, so C and the JPEG judge an
// absent, damaged, profile-less or profile-differing condition the same way.
struct FixedPreviewSource {
    std::optional<a0::m2::experimental::LoadedFixedCompositeCondition> loaded;
    std::optional<a0::m2::experimental::LoadedFixedCompositeConditionV2> loaded_v2;
    std::optional<a0::m2::experimental::LoadedFixedCompositeConditionV3> loaded_v3;
    a0::m2::experimental::FixedCompositeSkip reason{a0::m2::experimental::FixedCompositeSkip::condition_absent};
    std::string detail;
    // A condition file exists, usable or not.
    bool Present() const {
        return loaded.has_value() || loaded_v2.has_value() || loaded_v3.has_value() || reason!=a0::m2::experimental::FixedCompositeSkip::condition_absent;
    }
};
template<class Hash>
FixedPreviewSource LoadFixedPreviewSource(const std::filesystem::path& file,Hash hash){
    FixedPreviewSource source;
    auto selection=a0::m2::experimental::SelectFixedComposite(file,a0::m2::experimental::FixedCompositeSha256(hash));
    source.reason=selection.reason;source.detail=std::move(selection.detail);
    if(selection.loaded){
        if(auto* v1=std::get_if<a0::m2::experimental::LoadedFixedCompositeCondition>(&*selection.loaded))source.loaded=std::move(*v1);
        else if(auto* v2=std::get_if<a0::m2::experimental::LoadedFixedCompositeConditionV2>(&*selection.loaded))source.loaded_v2=std::move(*v2);
        else if(auto* v3=std::get_if<a0::m2::experimental::LoadedFixedCompositeConditionV3>(&*selection.loaded))source.loaded_v3=std::move(*v3);
        else{
            // A record of a kind that C does not know: nothing is loaded and the reason is kept, so C is not drawn with it and
            // says why. (LoadFixedCompositeConditionAny returns a v3 record only once the capture path reads v3.)
            source.reason=a0::m2::experimental::FixedCompositeSkip::condition_invalid;
            source.detail="fixed composite condition of an unknown kind is not drawn by the live view";
        }
    }
    return source;
}

// uncalibrated: no condition exists, the earlier translation-only C (manual and
// automatic alignment) stays. fixed_composite: drawn with the condition.
// unavailable: a condition exists but C cannot be drawn with it; C is not drawn
// and the reason is shown (ADR-0036 decision 1, item 8).
enum class PreviewCMode {uncalibrated,fixed_composite,unavailable};
struct FixedPreviewChoice {
    PreviewCMode mode{PreviewCMode::uncalibrated};
    FixedPreviewPlan plan; // fixed_composite: ready; unavailable: plan.skip is the reason (also when the file was damaged)
};
inline FixedPreviewChoice ChoosePreviewC(const FixedPreviewSource& source,a0::m2::experimental::MountOrientation orientation,
    unsigned view_width,unsigned view_height){
    FixedPreviewChoice choice;
    choice.plan.view_width=view_width;choice.plan.view_height=view_height;
    if(source.loaded_v2){
        choice.plan=PlanFixedPreviewV2(*source.loaded_v2,orientation,view_width,view_height);
        choice.mode=choice.plan.skip?PreviewCMode::unavailable:PreviewCMode::fixed_composite;
        return choice;
    }
    if(source.loaded_v3){
        choice.plan=PlanFixedPreviewV3(*source.loaded_v3,orientation,view_width,view_height);
        choice.mode=choice.plan.skip?PreviewCMode::unavailable:PreviewCMode::fixed_composite;
        return choice;
    }
    if(!source.loaded){
        if(source.reason==a0::m2::experimental::FixedCompositeSkip::condition_absent)return choice;
        choice.mode=PreviewCMode::unavailable;choice.plan.skip=source.reason;choice.plan.detail=source.detail;
        return choice;
    }
    choice.plan=PlanFixedPreview(source.loaded->condition,orientation,view_width,view_height);
    choice.mode=choice.plan.skip?PreviewCMode::unavailable:PreviewCMode::fixed_composite;
    return choice;
}

inline const wchar_t* FixedPreviewSeamOriginText(a0::m2::experimental::FixedSeamOrigin origin){
    return origin==a0::m2::experimental::FixedSeamOrigin::operator_set?L"人が決めた値":L"帯の中央の仮の値";
}
// Shown beside the uncalibrated C when there is no condition.
inline constexpr const wchar_t* kUncalibratedPreviewNotice=L"固定の合成条件がありません（未校正の仮の C）";
inline std::wstring FixedPreviewTitle(a0::m2::experimental::MountOrientation orientation,const FixedPreviewPlan& plan){
    (void)a0::m2::experimental::MountOrientationName(orientation);
    if(plan.live)
        return std::wstring(L"C 固定の合成条件 v3（姿勢と紙面を含む写像、目安・未承認・表示専用） 取付の向き: ")+
            (orientation==a0::m2::experimental::MountOrientation::rotated_180?L"180°回転":L"通常")+
            L" / 継ぎ目の列 "+std::to_wstring(plan.capture_seam_column)+L"（出力ラスタの画素・"+FixedPreviewSeamOriginText(plan.seam_origin)+L"）";
    if(plan.lens_aware)
        return std::wstring(L"C 固定の合成条件 v2（レンズの歪みを補正、未承認・表示専用） 取付の向き: ")+
            (orientation==a0::m2::experimental::MountOrientation::rotated_180?L"180°回転":L"通常")+
            L" / 継ぎ目の列 "+std::to_wstring(plan.capture_seam_column)+L"（出力ラスタの画素・"+FixedPreviewSeamOriginText(plan.seam_origin)+L"）";
    return std::wstring(L"C 固定の合成条件（未校正・未承認・表示専用） 取付の向き: ")+
        (orientation==a0::m2::experimental::MountOrientation::rotated_180?L"180°回転":L"通常")+
        L" / 継ぎ目の列 "+std::to_wstring(plan.capture_seam_column)+L"（撮影画像の画素・"+FixedPreviewSeamOriginText(plan.seam_origin)+L"）";
}
// The standing note under the title; the unverified correspondence is always named.
inline std::wstring FixedPreviewNote(const FixedPreviewPlan& plan){
    std::wstring text=L"ライブビューと撮影画像の対応は ※未検証（#309） / 倍率は補正しません / 保存しません";
    if(plan.lens_aware){
        // The profile id is the text of the profile file; only plain ASCII is shown.
        std::wstring id;
        if(plan.profile_id)for(const char c:*plan.profile_id)id+=(static_cast<unsigned char>(c)>=0x20 && static_cast<unsigned char>(c)<0x7f)?static_cast<wchar_t>(c):L'?';
        text=L"ライブビューと撮影画像の対応（S）は ※未検証（#309） / 校正の写像（draft、未承認）"+(id.empty()?std::wstring():L" "+id)+
            L" / 倍率は補正しません / 保存しません";
        // The registered C is a guide: it does not say that the picture is within any criterion.
        if(plan.live){
            text=L"ライブビューと撮影画像の対応（S）は ※未検証（#309） / 固定の合成条件 v3 の写像（基準の姿勢を含む、未承認）"+(id.empty()?std::wstring():L" "+id)+
                L" / 倍率は補正しません / 保存しません / Cは目安です";
            if(!plan.live->live_limits)text+=L" / 映像からの合わせ込みは条件で無効です";
        }
    }
    if(plan.seam_shift)text+=L" / 継ぎ目をCの画素に合わせて "+std::to_wstring(std::abs(plan.seam_shift))+L"px "+(plan.seam_shift<0?L"左":L"右")+L"へ寄せています";
    return text;
}
// The title while a condition file exists but C is not drawn with it.
inline std::wstring FixedPreviewUnavailableTitle(const FixedPreviewSource& source){
    if(source.loaded_v3)return L"C 固定の合成条件 v3（未承認・表示専用）";
    return source.loaded_v2?L"C 固定の合成条件 v2（未承認・表示専用）":L"C 固定の合成条件（未校正・未承認・表示専用）";
}
// A position request (manual or automatic) while C is drawn with the condition.
inline std::wstring FixedPreviewPositionRefusal(){
    return L"固定の合成条件で描いているため、位置合わせはCの位置を変えません（ライブビューと撮影画像の対応は ※未検証 #309）";
}
// Why C is not drawn although a condition file exists.
inline std::wstring FixedPreviewUnavailableText(a0::m2::experimental::FixedCompositeSkip skip){
    using S=a0::m2::experimental::FixedCompositeSkip;
    switch(skip){
    case S::condition_absent:return kUncalibratedPreviewNotice;
    case S::condition_invalid:return L"固定の合成条件を読めません（内容が正しくありません）。Cを描きません。設置の時に作り直してください";
    case S::orientation_differs:return L"固定の合成条件の取り付けの向きが、この画面の向きと合いません。Cを描きません";
    case S::size_differs:return L"固定の合成条件を、ライブビューの大きさへ縮められません。Cを描きません";
    case S::seam_outside_overlap:return L"固定の合成条件の継ぎ目の列が、縮めたCの重なりの外にあります。Cを描きません";
    // Worded as the pair capture's FixedCompositeSkipText, followed by what C does.
    case S::profile_absent:return L"固定の合成条件が参照する校正ファイル（profile）が、条件と同じフォルダにありません。Cを描きません";
    case S::profile_differs:return L"固定の合成条件が参照する校正ファイル（profile）が、条件の記録と違うか使えません。設置の時に作り直してください。Cを描きません";
    case S::pose_outside_envelope:return L"固定の合成条件の手直しが、範囲の外です。rollback するか、校正をやり直してください。Cを描きません";
    }
    throw std::invalid_argument("unknown fixed composite skip");
}
}
