#include "a0/m2/experimental/registration_probe.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace a0::m2::experimental::registration_probe {
namespace {
using render::Matrix3;
using render::Point;
using Vec3 = std::array<double,3>;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

void Require(bool condition,const char* message){
    if(!condition)throw std::invalid_argument(message);
}

Vec3 Apply(const Matrix3& m,const Vec3& v){
    return {m[0]*v[0]+m[1]*v[1]+m[2]*v[2],m[3]*v[0]+m[4]*v[1]+m[5]*v[2],m[6]*v[0]+m[7]*v[1]+m[8]*v[2]};
}
}

std::vector<Observation> ObservationsOf(const seam_gate::SeamMeasurement& m,Point shift){
    std::vector<Observation> out;
    for(const auto& p:m.points){
        if(!p.Counted())continue;
        Observation o;
        o.x=p.x;o.y=p.y;o.one_directional=p.kind==seam_gate::PointKind::one_directional;
        o.normal=p.normal;o.d={p.displacement.x+shift.x,p.displacement.y+shift.y};o.sigma_px=p.uncertainty_px;
        o.visible_px=o.one_directional?std::abs(o.normal.x*o.d.x+o.normal.y*o.d.y):std::hypot(o.d.x,o.d.y);
        out.push_back(o);
    }
    return out;
}

std::vector<Observation> DifferenceObservations(const std::vector<Observation>& before,const std::vector<Observation>& after){
    // Join by sample index.
    std::map<std::pair<double,double>,const Observation*> index;
    for(const auto& o:before)index[{o.x,o.y}]=&o;
    constexpr double kCosine=0.9961946980917455; // cos(5 degrees)
    std::vector<Observation> out;
    for(const auto& b:after){
        const auto found=index.find({b.x,b.y});
        if(found==index.end())continue;
        const auto& a=*found->second;
        Observation o;
        o.x=b.x;o.y=b.y;o.sigma_px=std::hypot(a.sigma_px,b.sigma_px);
        o.d={b.d.x-a.d.x,b.d.y-a.d.y};
        if(!a.one_directional && !b.one_directional){
            o.one_directional=false;
        }else if(a.one_directional && b.one_directional){
            if(std::abs(a.normal.x*b.normal.x+a.normal.y*b.normal.y)<kCosine)continue;
            o.one_directional=true;o.normal=a.normal;
        }else{
            o.one_directional=true;o.normal=a.one_directional?a.normal:b.normal;
        }
        o.visible_px=o.one_directional?std::abs(o.normal.x*o.d.x+o.normal.y*o.d.y):std::hypot(o.d.x,o.d.y);
        out.push_back(o);
    }
    return out;
}

// ---- Models ------------------------------------------------------------------------------
std::string_view ModelName(ModelKind kind){
    switch(kind){
    case ModelKind::none: return "none";
    case ModelKind::translation: return "translation-2";
    case ModelKind::similarity: return "similarity-4";
    case ModelKind::similarity_vertical_scale: return "similarity-vertical-scale-5";
    case ModelKind::pose6: return "pose-6";
    case ModelKind::projective8: return "projective-8";
    case ModelKind::polynomial2: return "polynomial2-12";
    }
    throw std::invalid_argument("unknown model");
}
unsigned ModelParameterCount(ModelKind kind){
    switch(kind){
    case ModelKind::none: return 0;
    case ModelKind::translation: return 2;
    case ModelKind::similarity: return 4;
    case ModelKind::similarity_vertical_scale: return 5;
    case ModelKind::pose6: return 6;
    case ModelKind::projective8: return 8;
    case ModelKind::polynomial2: return 12;
    }
    throw std::invalid_argument("unknown model");
}

namespace {
constexpr unsigned kMaximumParameters=12;
using Predictor=std::function<Point(double,double)>;
// A model: the typical size of each parameter (the fit works on parameter/scale, which keeps the
// normal equations balanced) and the map from parameters to a predictor.
struct ModelDefinition {
    unsigned count{};
    std::array<double,kMaximumParameters> scale{};
    std::function<Predictor(const double*)> bind;   // parameters in model units
};

ModelDefinition Define(ModelKind kind,const std::vector<Observation>& observations,const FitContext& context){
    double cx=0,cy=0;
    for(const auto& o:observations){cx+=o.x;cy+=o.y;}
    if(!observations.empty()){cx/=static_cast<double>(observations.size());cy/=static_cast<double>(observations.size());}
    ModelDefinition d;
    d.count=ModelParameterCount(kind);
    switch(kind){
    case ModelKind::none: break;
    case ModelKind::translation:
        d.scale={10,10};
        d.bind=[](const double* p){const double a=p[0],b=p[1];return Predictor([a,b](double,double){return Point{a,b};});};
        break;
    case ModelKind::similarity:
    case ModelKind::similarity_vertical_scale: {
        d.scale={10,10,1e-2,1e-2,1e-2};
        const bool vertical=kind==ModelKind::similarity_vertical_scale;
        // Displacement at the centre of the data, a scale change a, a rotation b (radians) and,
        // for the five-parameter model, an extra scale e along y.
        d.bind=[cx,cy,vertical](const double* p){
            const double tx=p[0],ty=p[1],a=p[2],b=p[3],e=vertical?p[4]:0.0;
            return Predictor([=](double x,double y){
                const double u=x-cx,v=y-cy;
                return Point{tx+a*u-b*v,ty+b*u+a*v+e*v};
            });
        };
        break;
    }
    case ModelKind::polynomial2:
        d.scale={10,10,10,10,10,10,10,10,10,10,10,10};
        d.bind=[cx,cy](const double* p){
            std::array<double,12> c{};
            std::copy(p,p+12,c.begin());
            return Predictor([c,cx,cy](double x,double y){
                const double u=(x-cx)/1000.0,v=(y-cy)/1000.0;
                const double basis[6]{1,u,v,u*u,u*v,v*v};
                Point r{};
                for(int k=0;k<6;++k){r.x+=c[k]*basis[k];r.y+=c[6+k]*basis[k];}
                return r;
            });
        };
        break;
    case ModelKind::pose6: {
        Require(context.pose!=nullptr,"registration probe: the pose model needs the camera's pose model");
        const PoseModel* model=context.pose;const PlaneFrame frame=context.frame;
        d.scale={1.0,1.0,1.0,10.0,10.0,1.0};
        d.bind=[model,frame](const double* p){
            const PoseParameters pose{p[0],p[1],p[2],p[3],p[4],p[5]};
            const auto g=model->Homography(pose);
            return Predictor([g,frame](double x,double y){
                const double px=frame.left_mm+(x+0.5)*frame.pitch_mm,py=frame.top_mm+(y+0.5)*frame.pitch_mm;
                const Vec3 moved=Apply(g,{px,py,1});
                return Point{(moved[0]/moved[2]-px)/frame.pitch_mm,(moved[1]/moved[2]-py)/frame.pitch_mm};
            });
        };
        break;
    }
    case ModelKind::projective8: {
        const PlaneFrame frame=context.frame;
        const double cxm=frame.left_mm+(cx+0.5)*frame.pitch_mm,cym=frame.top_mm+(cy+0.5)*frame.pitch_mm;
        d.scale={1e-2,1e-2,1e-2,1e-2,1e-2,1e-2,1e-2,1e-2};
        // Document coordinates are taken about the centre of the data and in units of 1000 mm.
        d.bind=[frame,cxm,cym](const double* p){
            std::array<double,8> h{};
            std::copy(p,p+8,h.begin());
            return Predictor([h,frame,cxm,cym](double x,double y){
                const double X=frame.left_mm+(x+0.5)*frame.pitch_mm,Y=frame.top_mm+(y+0.5)*frame.pitch_mm;
                const double u=(X-cxm)/1000.0,v=(Y-cym)/1000.0;
                const double w=h[6]*u+h[7]*v+1.0;
                const double mu=((1+h[0])*u+h[1]*v+h[2])/w,mv=(h[3]*u+(1+h[4])*v+h[5])/w;
                return Point{(mu-u)*1000.0/frame.pitch_mm,(mv-v)*1000.0/frame.pitch_mm};
            });
        };
        break;
    }
    }
    return d;
}

// Dense symmetric positive solve by Gaussian elimination with partial pivoting.
bool Solve(std::vector<double> a,std::vector<double> b,unsigned n,std::vector<double>& x){
    for(unsigned c=0;c<n;++c){
        unsigned pivot=c;
        for(unsigned r=c+1;r<n;++r)if(std::abs(a[r*n+c])>std::abs(a[pivot*n+c]))pivot=r;
        if(!(std::abs(a[pivot*n+c])>1e-300))return false;
        if(pivot!=c){for(unsigned k=0;k<n;++k)std::swap(a[c*n+k],a[pivot*n+k]);std::swap(b[c],b[pivot]);}
        for(unsigned r=c+1;r<n;++r){
            const double f=a[r*n+c]/a[c*n+c];
            for(unsigned k=c;k<n;++k)a[r*n+k]-=f*a[c*n+k];
            b[r]-=f*b[c];
        }
    }
    x.assign(n,0.0);
    for(int r=static_cast<int>(n)-1;r>=0;--r){
        double s=b[r];
        for(unsigned k=r+1;k<n;++k)s-=a[r*n+k]*x[k];
        x[r]=s/a[r*n+r];
    }
    return true;
}
bool Invert(const std::vector<double>& a,unsigned n,std::vector<double>& inverse){
    inverse.assign(static_cast<std::size_t>(n)*n,0.0);
    for(unsigned c=0;c<n;++c){
        std::vector<double> e(n,0.0),x;
        e[c]=1;
        if(!Solve(a,e,n,x))return false;
        for(unsigned r=0;r<n;++r)inverse[r*n+c]=x[r];
    }
    return true;
}

double ResidualMagnitude(const Observation& o,Point predicted){
    const double rx=o.d.x-predicted.x,ry=o.d.y-predicted.y;
    return o.one_directional?std::abs(o.normal.x*rx+o.normal.y*ry):std::hypot(rx,ry);
}
}

ModelFit FitModel(ModelKind kind,const std::vector<Observation>& observations,const FitContext& context,const FitOptions& options){
    ModelFit fit;
    fit.kind=kind;fit.observations=static_cast<unsigned>(observations.size());
    const auto zero=[](double,double){return Point{0,0};};
    fit.predict=zero;
    if(kind==ModelKind::none){fit.fitted=fit.converged=true;fit.inliers=fit.observations;fit.weight.assign(observations.size(),1.0);return fit;}
    const auto def=Define(kind,observations,context);
    const unsigned n=def.count;
    unsigned rows_total=0;
    for(const auto& o:observations)rows_total+=o.one_directional?1u:2u;
    if(rows_total<2*n || observations.size()<n)return fit;
    fit.fitted=true;

    std::vector<double> q(n,0.0);
    std::vector<double> rho(observations.size(),1.0);
    const auto sigma_of=[&](const Observation& o){return std::max(o.sigma_px,options.sigma_floor_px);};
    const auto parameters_of=[&](const std::vector<double>& qq){
        std::array<double,kMaximumParameters> p{};
        for(unsigned i=0;i<n;++i)p[i]=qq[i]*def.scale[i];
        return p;
    };
    const auto cost_of=[&](const std::vector<double>& qq){
        const auto p=parameters_of(qq);
        const auto predictor=def.bind(p.data());
        double cost=0;
        for(std::size_t i=0;i<observations.size();++i){
            if(rho[i]<=0)continue;
            const auto& o=observations[i];
            const auto pred=predictor(o.x,o.y);
            const double rx=o.d.x-pred.x,ry=o.d.y-pred.y;
            const double s=sigma_of(o);
            const double sq=o.one_directional?std::pow(o.normal.x*rx+o.normal.y*ry,2):rx*rx+ry*ry;
            cost+=rho[i]*sq/(s*s);
        }
        return cost;
    };
    // Residual rows and Jacobian by central differences in q (the models are smooth).
    const auto linearize=[&](const std::vector<double>& qq,std::vector<double>& normal,std::vector<double>& gradient,double& cost){
        const auto p=parameters_of(qq);
        const auto base=def.bind(p.data());
        constexpr double kStep=1e-4;
        std::vector<Predictor> plus(n),minus(n);
        for(unsigned k=0;k<n;++k){
            auto qp=qq,qm=qq;qp[k]+=kStep;qm[k]-=kStep;
            const auto pp=parameters_of(qp),pm=parameters_of(qm);
            plus[k]=def.bind(pp.data());minus[k]=def.bind(pm.data());
        }
        normal.assign(static_cast<std::size_t>(n)*n,0.0);gradient.assign(n,0.0);cost=0;
        for(std::size_t i=0;i<observations.size();++i){
            if(rho[i]<=0)continue;
            const auto& o=observations[i];
            const double s=sigma_of(o),w=rho[i]/(s*s);
            const auto pred=base(o.x,o.y);
            double r[2]{o.d.x-pred.x,o.d.y-pred.y};
            double j[2][kMaximumParameters]{};
            for(unsigned k=0;k<n;++k){
                const auto a=plus[k](o.x,o.y),b=minus[k](o.x,o.y);
                j[0][k]=(a.x-b.x)/(2*kStep);j[1][k]=(a.y-b.y)/(2*kStep);
            }
            if(o.one_directional){
                const double rn=o.normal.x*r[0]+o.normal.y*r[1];
                double jn[kMaximumParameters]{};
                for(unsigned k=0;k<n;++k)jn[k]=o.normal.x*j[0][k]+o.normal.y*j[1][k];
                cost+=w*rn*rn;
                for(unsigned a=0;a<n;++a){
                    gradient[a]+=w*jn[a]*rn;
                    for(unsigned b=0;b<n;++b)normal[a*n+b]+=w*jn[a]*jn[b];
                }
            }else{
                cost+=w*(r[0]*r[0]+r[1]*r[1]);
                for(unsigned a=0;a<n;++a){
                    gradient[a]+=w*(j[0][a]*r[0]+j[1][a]*r[1]);
                    for(unsigned b=0;b<n;++b)normal[a*n+b]+=w*(j[0][a]*j[0][b]+j[1][a]*j[1][b]);
                }
            }
        }
    };

    double mu=1e-3;
    std::vector<double> last_normal;
    double last_cost=0;
    const auto run_inner=[&](){
        bool settled=false;
        for(unsigned it=0;it<options.maximum_inner_iterations;++it){
            ++fit.iterations;
            std::vector<double> normal,gradient;double cost{};
            linearize(q,normal,gradient,cost);
            last_normal=normal;last_cost=cost;
            bool improved=false;
            for(int attempt=0;attempt<30;++attempt){
                auto damped=normal;
                for(unsigned k=0;k<n;++k)damped[k*n+k]+=mu*std::max(normal[k*n+k],1e-12);
                std::vector<double> step;
                if(!Solve(damped,gradient,n,step)){mu*=10;continue;}
                auto trial=q;
                double largest=0;
                for(unsigned k=0;k<n;++k){trial[k]+=step[k];largest=std::max(largest,std::abs(step[k]));}
                const double trial_cost=cost_of(trial);
                if(std::isfinite(trial_cost) && trial_cost<=cost){
                    q=trial;mu=std::max(mu/3,1e-12);improved=true;
                    if(largest<1e-9 || cost-trial_cost<=1e-13*std::max(cost,1.0))settled=true;
                    break;
                }
                mu*=4;
            }
            if(!improved){settled=true;}
            if(settled)break;
        }
        return settled;
    };

    bool converged=false;
    double scale=options.scale_floor_px;
    for(unsigned outer=0;outer<options.maximum_outer_iterations;++outer){
        const bool inner_settled=run_inner();
        // Residual magnitudes with the current parameters, over all observations.
        const auto p=parameters_of(q);
        const auto predictor=def.bind(p.data());
        std::vector<double> magnitude(observations.size());
        for(std::size_t i=0;i<observations.size();++i)magnitude[i]=ResidualMagnitude(observations[i],predictor(observations[i].x,observations[i].y));
        auto sorted=magnitude;
        std::nth_element(sorted.begin(),sorted.begin()+sorted.size()/2,sorted.end());
        scale=std::max(1.4826*sorted[sorted.size()/2],options.scale_floor_px);
        const double c=options.tukey_constant*scale;
        double change=0;
        for(std::size_t i=0;i<observations.size();++i){
            const double t=magnitude[i]/c;
            const double next=t<1?std::pow(1-t*t,2):0.0;
            change=std::max(change,std::abs(next-rho[i]));
            rho[i]=next;
        }
        if(outer>0 && change<1e-6 && inner_settled){converged=true;break;}
    }
    // Final linearization with the final weights, for the parameter uncertainty.
    {
        std::vector<double> normal,gradient;double cost{};
        linearize(q,normal,gradient,cost);
        last_normal=normal;last_cost=cost;
    }
    fit.converged=converged;
    fit.robust_scale_px=scale;
    fit.inliers=0;fit.weight=rho;
    unsigned inlier_rows=0;
    for(std::size_t i=0;i<observations.size();++i)if(rho[i]>0){++fit.inliers;inlier_rows+=observations[i].one_directional?1u:2u;}
    const auto p=parameters_of(q);
    fit.parameters.assign(p.begin(),p.begin()+n);
    fit.predict=def.bind(p.data());
    if(inlier_rows>n){
        std::vector<double> inverse;
        if(Invert(last_normal,n,inverse)){
            const double s2=std::max(1.0,last_cost/static_cast<double>(inlier_rows-n));
            fit.parameter_sigma.resize(n);
            for(unsigned k=0;k<n;++k)fit.parameter_sigma[k]=std::sqrt(std::max(0.0,inverse[k*n+k])*s2)*def.scale[k];
        }
    }
    if(kind==ModelKind::pose6){
        fit.pose=PoseParameters{fit.parameters[0],fit.parameters[1],fit.parameters[2],fit.parameters[3],fit.parameters[4],fit.parameters[5]};
        if(fit.parameter_sigma.size()==6)
            fit.pose_sigma=PoseParameters{fit.parameter_sigma[0],fit.parameter_sigma[1],fit.parameter_sigma[2],fit.parameter_sigma[3],fit.parameter_sigma[4],fit.parameter_sigma[5]};
    }
    return fit;
}

double ResidualOf(const Observation& o,const ModelFit& fit){
    const Point predicted=fit.predict?fit.predict(o.x,o.y):Point{0,0};
    return ResidualMagnitude(o,predicted);
}

ResidualStats StatsOf(std::vector<double> values){
    ResidualStats s;
    s.count=static_cast<unsigned>(values.size());
    if(values.empty())return s;
    std::sort(values.begin(),values.end());
    double sum=0,squares=0;
    for(const double v:values){sum+=v;squares+=v*v;}
    s.mean=sum/static_cast<double>(values.size());
    s.rms=std::sqrt(squares/static_cast<double>(values.size()));
    const auto rank=static_cast<std::size_t>(std::ceil(0.95*static_cast<double>(values.size())));
    s.p95=values[std::max<std::size_t>(rank,1)-1];
    s.maximum=values.back();
    return s;
}

SeamColumnSeries SeamColumnResidual(const std::vector<Observation>& observations,const std::vector<double>& residuals,
    unsigned seam_column,unsigned lattice_step,unsigned height){
    Require(observations.size()==residuals.size(),"registration probe: one residual per observation is required");
    Require(lattice_step>0,"registration probe: invalid lattice step");
    SeamColumnSeries series;
    series.residual.assign(height,kNaN);
    // Per lattice row, the observation nearest the seam column within one step.
    struct Sample {double y{},value{},distance{};};
    std::vector<Sample> samples;
    for(std::size_t i=0;i<observations.size();++i){
        const auto& o=observations[i];
        const double distance=std::abs(o.x-static_cast<double>(seam_column));
        if(distance>static_cast<double>(lattice_step))continue;
        auto found=std::find_if(samples.begin(),samples.end(),[&](const Sample& s){return s.y==o.y;});
        if(found==samples.end())samples.push_back({o.y,residuals[i],distance});
        else if(distance<found->distance)*found={o.y,residuals[i],distance};
    }
    std::sort(samples.begin(),samples.end(),[](const Sample& a,const Sample& b){return a.y<b.y;});
    const auto row_of=[&](double y)->long{return static_cast<long>(std::llround(y));};
    for(std::size_t k=0;k<samples.size();++k){
        const long r=row_of(samples[k].y);
        if(r>=0 && r<static_cast<long>(height))series.residual[r]=samples[k].value;
        if(k+1==samples.size())break;
        const auto& a=samples[k];const auto& b=samples[k+1];
        if(b.y-a.y>2.0*lattice_step)continue;
        for(long y=row_of(a.y)+1;y<row_of(b.y);++y){
            if(y<0 || y>=static_cast<long>(height))continue;
            const double t=(static_cast<double>(y)-a.y)/(b.y-a.y);
            series.residual[y]=a.value+t*(b.value-a.value);
        }
    }
    std::vector<double> covered;
    for(const double v:series.residual)if(!std::isnan(v))covered.push_back(v);
    series.covered_rows=static_cast<unsigned>(covered.size());
    series.stats=StatsOf(covered);
    return series;
}

ResidualShape ShapeOf(const SeamColumnSeries& series){
    ResidualShape shape;
    long first=-1,last=-1;
    for(std::size_t y=0;y<series.residual.size();++y)if(!std::isnan(series.residual[y])){if(first<0)first=static_cast<long>(y);last=static_cast<long>(y);}
    if(first<0)return shape;
    const double span=static_cast<double>(last-first+1);
    double sum[3]{},count[3]{},total=0,ends=0;
    for(long y=first;y<=last;++y){
        const double v=series.residual[static_cast<std::size_t>(y)];
        if(std::isnan(v))continue;
        const double position=static_cast<double>(y-first)/span;
        const int third=std::min(2,static_cast<int>(position*3));
        sum[third]+=v;count[third]+=1;
        total+=v*v;
        if(position<0.2 || position>=0.8)ends+=v*v;
    }
    shape.top_third_mean=count[0]>0?sum[0]/count[0]:kNaN;
    shape.middle_third_mean=count[1]>0?sum[1]/count[1]:kNaN;
    shape.bottom_third_mean=count[2]>0?sum[2]/count[2]:kNaN;
    shape.end_share=total>0?ends/total:0.0;
    return shape;
}

seam_gate::SeamGateDesign FieldDesign(unsigned point_spacing_px,unsigned global_maximum_evaluations){
    auto design=seam_gate::kSeamGateDesignCalibrated;
    design.point_spacing_px=point_spacing_px;
    design.block_px=2*point_spacing_px;
    design.strip_rows=design.block_px*((256+design.block_px-1)/design.block_px);
    design.global_maximum_evaluations=global_maximum_evaluations;
    design.global_minimum_anchors=std::min(design.global_minimum_anchors,global_maximum_evaluations);
    seam_gate::ValidateSeamGateDesign(design);
    return design;
}

OutputInverse::OutputInverse(render::OutputToCamera map,std::uint32_t width,std::uint32_t height,unsigned grid_step_px)
    :map_(std::move(map)),width_(width),height_(height),step_(grid_step_px){
    Require(static_cast<bool>(map_) && width_>=2 && height_>=2 && step_>=2 && step_<=4096,"output inverse: mapping, raster and grid step are required");
    columns_=(width_-1)/step_+2;rows_=(height_-1)/step_+2;
    nodes_.assign(static_cast<std::size_t>(columns_)*rows_,Point{kNaN,kNaN});
    for(std::uint32_t j=0;j<rows_;++j)for(std::uint32_t i=0;i<columns_;++i){
        const Point o{std::min<double>(static_cast<double>(i)*step_,width_-1),std::min<double>(static_cast<double>(j)*step_,height_-1)};
        Point raw{};
        if(map_(o,raw) && std::isfinite(raw.x) && std::isfinite(raw.y))nodes_[static_cast<std::size_t>(j)*columns_+i]=raw;
    }
}

std::optional<Point> OutputInverse::Find(Point raw,double tolerance_px) const {
    Require(std::isfinite(raw.x) && std::isfinite(raw.y) && std::isfinite(tolerance_px) && tolerance_px>0,"output inverse: the raw position is not finite");
    // The nearest node in raw pixels (the grid is small: a few tens of thousands of nodes).
    double best=std::numeric_limits<double>::infinity();std::size_t at=nodes_.size();
    for(std::size_t k=0;k<nodes_.size();++k){
        const auto& n=nodes_[k];
        if(!std::isfinite(n.x))continue;
        const double d=std::hypot(n.x-raw.x,n.y-raw.y);
        if(d<best){best=d;at=k;}
    }
    if(at==nodes_.size())return std::nullopt;
    Point o{std::min<double>(static_cast<double>(at%columns_)*step_,width_-1),std::min<double>(static_cast<double>(at/columns_)*step_,height_-1)};
    const auto eval=[&](Point p,Point& r){return map_(p,r) && std::isfinite(r.x) && std::isfinite(r.y);};
    constexpr double h=0.5;
    for(int iteration=0;iteration<40;++iteration){
        Point r{},xp{},xm{},yp{},ym{};
        if(!eval(o,r))return std::nullopt;
        const double ex=r.x-raw.x,ey=r.y-raw.y;
        if(std::hypot(ex,ey)<=tolerance_px){
            if(o.x<0 || o.y<0 || o.x>width_-1 || o.y>height_-1)return std::nullopt;
            return o;
        }
        if(!eval({o.x+h,o.y},xp) || !eval({o.x-h,o.y},xm) || !eval({o.x,o.y+h},yp) || !eval({o.x,o.y-h},ym))return std::nullopt;
        const double j00=(xp.x-xm.x)/(2*h),j10=(xp.y-xm.y)/(2*h),j01=(yp.x-ym.x)/(2*h),j11=(yp.y-ym.y)/(2*h);
        const double det=j00*j11-j01*j10;
        if(!(std::abs(det)>1e-12))return std::nullopt;
        Point step{(j11*ex-j01*ey)/det,(-j10*ex+j00*ey)/det};
        // A step longer than the grid means the start was far off; keep it to the grid so Newton does not jump away.
        const double length=std::hypot(step.x,step.y);
        if(length>step_){step.x*=step_/length;step.y*=step_/length;}
        o.x-=step.x;o.y-=step.y;
    }
    return std::nullopt;
}

SeamComponents SeamComponentsOf(const seam_gate::SeamMeasurement& m,std::uint32_t seam_column,std::uint32_t half_width_px,Point e_b,double c_min){
    Require(std::isfinite(e_b.x) && std::isfinite(e_b.y) && std::abs(std::hypot(e_b.x,e_b.y)-1)<=1e-9,"seam components: e_b is not a unit vector");
    Require(std::isfinite(c_min) && c_min>0 && c_min<=1,"seam components: c_min is not in (0, 1]");
    const Point e_perp{-e_b.y,e_b.x};
    struct Sum {ComponentStats* stats;double square{};};
    SeamComponents out;
    Sum sums[5]{{&out.two_along},{&out.two_across},{&out.two_magnitude},{&out.one_with_baseline},{&out.one_across_baseline}};
    const auto add=[](Sum& s,double v){++s.stats->count;s.stats->maximum_abs=std::max(s.stats->maximum_abs,std::abs(v));s.square+=v*v;};
    for(const auto& p:m.points){
        const auto distance=p.x>seam_column?p.x-seam_column:seam_column-p.x;
        if(distance>half_width_px || p.status==seam_gate::PointStatus::outside_coverage)continue;
        if(!p.Counted()){++out.not_counted;continue;}
        ++out.counted;
        const auto& d=p.displacement;
        if(p.kind==seam_gate::PointKind::two_directional){
            add(sums[0],e_b.x*d.x+e_b.y*d.y);add(sums[1],e_perp.x*d.x+e_perp.y*d.y);add(sums[2],std::hypot(d.x,d.y));
        }else{
            const double along_normal=p.normal.x*d.x+p.normal.y*d.y;
            add(std::abs(p.normal.x*e_b.x+p.normal.y*e_b.y)>=c_min?sums[3]:sums[4],along_normal);
        }
    }
    for(auto& s:sums)if(s.stats->count)s.stats->rms=std::sqrt(s.square/s.stats->count);
    return out;
}
}
