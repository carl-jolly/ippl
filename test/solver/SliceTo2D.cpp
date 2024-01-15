//mpirun -np 1 test/solver/./SliceTo2D 128 128 128 10000 HOCKNEY --info 5

#include "Ippl.h"

#include <chrono>
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <vector>
#include <fstream>
#include <map>


#include "Utility/IpplException.h"
#include "Utility/IpplTimings.h"
#include "PoissonSolvers/FFTOpenPoissonSolver.h"

template <unsigned Dim>
using Mesh_t = ippl::UniformCartesian<double, Dim>;

template <typename T, unsigned Dim>
using PLayout_t = typename ippl::ParticleSpatialLayout<T, Dim, Mesh_t<Dim>>;

template <unsigned Dim>
using Centering_t = typename Mesh_t<Dim>::DefaultCentering;

template <unsigned Dim>
using FieldLayout_t = ippl::FieldLayout<Dim>;

using size_type = ippl::detail::size_type;

template <typename T, unsigned Dim>
using Vector = ippl::Vector<T, Dim>;

template <typename T, unsigned Dim, class... ViewArgs>
using Field = ippl::Field<T, Dim, Mesh_t<Dim>, Centering_t<Dim>, ViewArgs...>;

template <typename T>
using ParticleAttrib = ippl::ParticleAttrib<T>;

template <typename T, unsigned Dim>
using Vector_t = ippl::Vector<T, Dim>;

template <unsigned Dim, class... ViewArgs>
using Field_t = Field<double, Dim, ViewArgs...>;

//template <typename T, unsigned Dim, class... ViewArgs>
//using VField_t = Field<Vector_t<T, Dim>, Dim, ViewArgs...>;
template <typename T>
using VectorField_t = typename ippl::Field<ippl::Vector<T, 2>, 2, Mesh_t<2>, Centering_t<2>>;

template <typename T>
using ScalarField_t = typename ippl::Field<T, 2, Mesh_t<2>, Centering_t<2>>;

template <typename T>
using Solver_t = ippl::FFTOpenPoissonSolver<VectorField_t<T>, ScalarField_t<T>>;

template <class PLayout>
class Solve25D : public ippl::ParticleBase<PLayout> {
    using Base = ippl::ParticleBase<PLayout>;

public:

    Field<double, 2> phi_m;

    std::array<bool, 3> isParallel_m;

    Vector_t<double, 3> hr_m;
    Vector_t<double, 3> rmin_m;
    Vector_t<double, 3> rmax_m;

    std::string stype_m;

    double Q_m;

public:
    typedef double value_type;

    ParticleAttrib<double> q;                 // charge
    double binWidth_m;                        // width of longitudinal bin
    double globalMin_m;                       // min longitudinal coordinate across all ranks
    typename Base::particle_position_type P;  // particle velocity
    typename Base::particle_position_type E;  // electric field at particle position
    ParticleAttrib<int> mapping;              // record the bin index of the i'th particle


    Solve25D(PLayout& pl, Vector_t<double, 3>  hr, Vector_t<double, 3>  rmin, Vector_t<double, 3>  rmax,
                     std::array<bool, 3> isParallel, double Q, double binWidth ,double globalMin, std::string solver)
        : Base(pl) 
        , hr_m(hr)
        , rmin_m(rmin)
        , rmax_m(rmax)
        , stype_m(solver)
        , Q_m(Q) 
        , binWidth_m(binWidth) 
        , globalMin_m(globalMin) {
        registerAttributes();
        for (unsigned int i = 0; i < 3; i++) {
            isParallel_m[i] = isParallel[i];
        }
    }

    void registerAttributes() {
        // register the particle attributes
        this->addAttribute(q);
        this->addAttribute(P);
        this->addAttribute(E);
        this->addAttribute(mapping);
    }

    ~Solve25D() {}

    void gatherStatistics(unsigned int totalP) {
        unsigned int Total_particles = 0;
        unsigned int local_particles = this->getLocalNum();

        MPI_Reduce(&local_particles, &Total_particles, 1, MPI_UNSIGNED, MPI_SUM, 0,
                   ippl::Comm->getCommunicator());

        if (ippl::Comm->rank() == 0) {
            if (Total_particles != totalP) {
                std::cout << "Total particles in the sim. " << totalP << " "
                          << "after update: " << Total_particles << std::endl;
                exit(1);
            }
        }

        ippl::Comm->barrier();

        std::cout << "Rank " << ippl::Comm->rank() << " has " << local_particles << std::endl;
    }

    template <size_t nSlices>
    void scatterToSlices(std::array<ScalarField_t<double>, nSlices>& rhos, Kokkos::Array<Field_t<2>::view_type, nSlices>& rhoViews) {

        // copy stuff from particleattrib.hpp
        static IpplTimings::TimerRef scatterTimer = IpplTimings::getTimer("scatter");
        IpplTimings::startTimer(scatterTimer);

        using mesh_type = typename Field_t<2>::Mesh_t;
        mesh_type& mesh = rhos[0].get_mesh(); // mesh is the same for all rho

        using vector_type = typename mesh_type::vector_type;
        const vector_type& dx = mesh.getMeshSpacing();

        FieldLayout_t<2>& layout = rhos[0].getLayout();

        const vector_type& origin = mesh.getOrigin();

        const vector_type invdx      = 1.0 / dx;
        const ippl::NDIndex<2>& lDom = layout.getLocalNDIndex();
        const int nghost             = rhos[0].getNghost();

        auto Rview = this->R.getView();
        auto mappingView = mapping.getView();
        auto qView = q.getView();

        //double sMax, sMin;
        // parallel min/max loop (will be strictly necessary when the position data is on GPUs)
        //Kokkos::parallel_reduce(
        //    "Find min and max s coordinate", this->getLocalNum(),
        //    KOKKOS_LAMBDA(size_t particleIndex, double& localMax, double& localMin) {
        //        auto position = Rview(particleIndex);
        //        auto s = position[2];
        //        if (s < localMin) localMin = s;
        //        if (s > localMax) localMax = s;
        //    },
        //    Kokkos::Max<double>(sMax), Kokkos::Min<double>(sMin)
        //);
        // may or may not be necessary depending on final design, but just in case,
        // make sure you have the indices before continuing
        //Kokkos::fence();

        auto binWidth = this->binWidth_m;
        auto globalMin = this->globalMin_m;
        std::cerr << "globalMin = " << globalMin << std::endl;

        std::cerr << "binWidth = " << binWidth << std::endl;
       
        Kokkos::parallel_for(
            "Slice and scatter", this->getLocalNum(), KOKKOS_LAMBDA(const size_t i) {

                // mapping to record which particle corresponds to which bin 
                // ie the i'th particle corresponds to the n'th bin
                unsigned int n = (Rview(i)[2] - globalMin) / binWidth; 

                mappingView(i) = n;

                vector_type pos2D = {Rview(i)[0], Rview(i)[1]}; 

                vector_type l = (pos2D - origin) * invdx + 0.5;
                Vector<int, 2> index = l;
                Vector<double, 2> whi = l - index;
                Vector<double, 2> wlo = 1.0 - whi;
                Vector<size_t, 2> args = index - lDom.first() + nghost;

                const value_type &val = qView(i);

                ippl::detail::scatterToField(std::make_index_sequence<1 << 2>{}, rhoViews[n], wlo, whi, args, val);
            });
        IpplTimings::stopTimer(scatterTimer);

        // Write mapping values to a file
        std::ofstream outFile("mapping_output.csv");
        if (outFile.is_open()) {
            outFile << "index,n\n";
            for (size_t i = 0; i < this->getLocalNum(); i++) {
                outFile << i << "," << mappingView(i) << "\n";
            }
            outFile.close();
        } else {
            std::cerr << "Unable to open file for writing." << std::endl;
        }
    }

    template <size_t nSlices>
    void gatherFromSlices(std::array<ScalarField_t<double>, nSlices>& rhos, Kokkos::Array<VectorField_t<double>::view_type, nSlices>& EViews) {
        static IpplTimings::TimerRef gatherTimer = IpplTimings::getTimer("gather");
        IpplTimings::startTimer(gatherTimer);

        using mesh_type = typename Field_t<2>::Mesh_t;
        mesh_type& mesh = rhos[0].get_mesh(); // mesh is the same for all rho

        using vector_type = typename mesh_type::vector_type;
        const vector_type& dx = mesh.getMeshSpacing();

        FieldLayout_t<2>& layout = rhos[0].getLayout();

        const vector_type& origin = mesh.getOrigin();

        const vector_type invdx      = 1.0 / dx;
        const ippl::NDIndex<2>& lDom = layout.getLocalNDIndex();
        const int nghost             = rhos[0].getNghost();

        auto Rview = this->R.getView();
        auto mappingView = mapping.getView();
        auto Eview = E.getView();

        Kokkos::parallel_for(
            "Gather and assign E", this->getLocalNum(), KOKKOS_LAMBDA(const size_t i) {

                vector_type pos2D = {Rview(i)[0], Rview(i)[1]}; 

                vector_type l = (pos2D - origin) * invdx + 0.5;
                Vector<int, 2> index = l;
                Vector<double, 2> whi = l - index;
                Vector<double, 2> wlo = 1.0 - whi;
                Vector<size_t, 2> args = index - lDom.first() + nghost;

                unsigned int n = mappingView(i);

                Vector<double, 2> Efield2D;
                Efield2D = ippl::detail::gatherFromField(std::make_index_sequence<1 << 2>{}, EViews[n], wlo, whi, args);

                Eview(i)[0] = Efield2D[0];
                Eview(i)[1] = Efield2D[1];
            });
            
        IpplTimings::stopTimer(gatherTimer);

    }
};


ippl::Vector<double, 3> generateRandomPointOnCylinder(double centerX, double centerY, double radius, double height) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<double> angle_distribution(0.0, 2 * Kokkos::numbers::pi_v<double>);
    //std::uniform_real_distribution<double> height_distribution(0.0, height);
    std::normal_distribution<double> height_distribution(height/4, 0.05);
    std::uniform_real_distribution<double> radius_distribution(0.0, radius);

    double theta = angle_distribution(gen);
    double l = height_distribution(gen);
    double r = radius_distribution(gen);

    double x = centerX + r * std::cos(theta);
    double y = centerY + r * std::sin(theta);

    return {x, y, l};
}

ippl::Vector<double, 3> generateRandomPointOnEllipsoid(double centreX, double centreY,double centerZ, double a, double b, double c) {
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<double> distribution(0.0, 1.0);
    
    double theta = 2 * M_PI * distribution(gen);
    double phi = std::acos(2 * distribution(gen) - 1);

    double x = a * std::sin(phi) * std::cos(theta) +centreX;
    double y = b * std::sin(phi) * std::sin(theta)+centreY;
    double z = c * std::cos(phi)+centerZ;


    return {x, y, z};
}

int main(int argc, char* argv[]) {
    ippl::initialize(argc, argv);
    {
        Inform msg(argv[0]);
        Inform msg2all(argv[0], INFORM_ALL_NODES);

        ippl::Vector<int, 3> nr = {std::atoi(argv[1]), std::atoi(argv[2]), std::atoi(argv[3])};

        static IpplTimings::TimerRef mainTimer = IpplTimings::getTimer("mainTimer");
        IpplTimings::startTimer(mainTimer);
        //auto start                = std::chrono::high_resolution_clock::now();
        const unsigned int totalP = std::atoi(argv[4]);

        msg << "benchmarkUpdate" << endl
            << " Np= " << totalP << " grid = " << nr << endl;
        using bunch_type = Solve25D<PLayout_t<double, 3>>;

        std::unique_ptr<bunch_type> P;

        ippl::NDIndex<3> domain;
        for (unsigned i = 0; i < 3; i++) {
            domain[i] = ippl::Index(nr[i]);
        }

        std::array<bool, 3> isParallel = {false, false, true};
        // Only want to divide up the domain along the beam axis
        // i.e. ippl will divde up along the axis of the beam so that 
        // if, for example, there are two ranks then the first rank will get the 
        // the slices from s = 0 to s = (1/2)smax and the second rank will get the rest

        // create mesh and layout objects for this problem domain
        Vector_t<double, 3> rmin(0.0);
        Vector_t<double, 3> rmax(1.0);
        double dx       = rmax[0] / double(nr[0]);
        double dy       = rmax[1] / double(nr[1]);
        double dz       = rmax[2] / double(nr[2]);
        Vector_t<double, 3> hr     = {dx, dy, dz};
        Vector_t<double, 3> origin = {rmin[0], rmin[1], rmin[2]};

        Mesh_t<3> mesh(domain, hr, origin); // this has dim 3 as this is the 3D particle distribution
        FieldLayout_t<3> FL(MPI_COMM_WORLD, domain, isParallel);
        PLayout_t<double, 3> PL(FL, mesh);

        std::string solver = argv[5];   
        const size_t numSlices = 10; //std::atoi(argv[6]);

        double globalMin = 0.1;
        double globalMax = 0.9;
        double binWidth = (globalMax - globalMin) / numSlices;
        double Q = 1.0;
        P        = std::make_unique<bunch_type>(PL, hr, rmin, rmax, isParallel, Q, binWidth ,globalMin, solver);

        unsigned long int nloc = totalP / ippl::Comm->size();

        static IpplTimings::TimerRef particleCreation = IpplTimings::getTimer("particlesCreation");
        IpplTimings::startTimer(particleCreation);
        P->create(nloc);

        typename bunch_type::particle_position_type::HostMirror R_host = P->R.getHostMirror();

        double radius = 0.05;
        std::ofstream outfile("distribution.csv");
        outfile << "x,y,z\n";

        if (!outfile) {
            std::cerr << "Error opening the output file." << std::endl;
            return 1;
        }

        double sum_coord = 0.0;
        double centreX = (rmin[0] + rmax[0]) / 2; // make sure that the test distribution in centred on the mesh origin
        double centreY = (rmin[1] + rmax[1]) / 2;
        for (unsigned long int i = 0; i < nloc; i++) {
            //std::vector<double> point = generateRandomPointOnCylinder(centreX, centreY, radius, (globalMax -globalMin)/2);
            ippl::Vector<double, 3> point = generateRandomPointOnEllipsoid(centreX, centreY, 0.5, radius, radius, (globalMax -globalMin)/2);            
            for (int d = 0; d < 3; d++) {
                R_host(i)[d] = point[d];
                sum_coord += R_host(i)[d];
            }
            outfile << point[0] << "," << point[1] << "," << point[2] << "\n";
        }

        double global_sum_coord = 0.0;
        MPI_Reduce(&sum_coord, &global_sum_coord, 1, MPI_DOUBLE, MPI_SUM, 0,
                   ippl::Comm->getCommunicator());

        if (ippl::Comm->rank() == 0) {
            std::cout << "Sum Coord: " << std::setprecision(16) << global_sum_coord << std::endl;
        }

        Kokkos::deep_copy(P->R.getView(), R_host);
        P->q = P->Q_m;

        IpplTimings::stopTimer(particleCreation);
        P->E = 0.0;

        static IpplTimings::TimerRef UpdateTimer = IpplTimings::getTimer("ParticleUpdate");
        IpplTimings::startTimer(UpdateTimer);
        P->update();
        IpplTimings::stopTimer(UpdateTimer);

        P->gatherStatistics(totalP);

        msg << "particles created and initial conditions assigned " << endl;

        // create mesh and initialise fields
        ippl::Index I(nr[0]);
        ippl::NDIndex<2> owned(I, I);

        // unit box
        double dxRho                      = 1.0 / nr[0];
        double dyRho                      = 1.0 / nr[1];
        ippl::Vector<double, 2> hxRho     = {dxRho, dyRho};
        ippl::Vector<double, 2> originRho = {0.0, 0.0};
        std::array<bool, 2> isParallelRho = {false, false};

        Mesh_t<2> meshRho(owned, hxRho, originRho);

        FieldLayout_t<2> layout(MPI_COMM_WORLD, owned, isParallelRho);  // 0 and 1 th element of decomp is serial
        
        std::array<ScalarField_t<double>, numSlices> rhos;
        Kokkos::Array<ScalarField_t<double>::view_type, numSlices> rhoViews;

        std::array<VectorField_t<double>, numSlices> Efields;
        Kokkos::Array<VectorField_t<double>::view_type, numSlices> EViews;

        for (size_t i = 0; i < numSlices; i++) {
            rhos[i] = 0.0; // reset rho field 

            rhos[i].initialize(meshRho, layout);
            rhoViews[i] = rhos[i].getView();

            Efields[i].initialize(meshRho, layout);
            EViews[i] = Efields[i].getView();
        }

        // call slice and scatter function
        P->scatterToSlices(rhos, rhoViews); 
        // 2d solve 
        ippl::ParameterList params;

        // set the FFT parameters
        params.add("use_heffte_defaults", false);
        params.add("use_pencils", true);
        params.add("use_gpu_aware", true);
        params.add("comm", ippl::a2av);
        params.add("r2c_direction", 0);

        // define an FFTPoissonSolver object
        std::ofstream inputFileRho("input_rho.csv");

        // Write the view data to the output file
        inputFileRho << "i,j,val\n";
        for (size_t i = 0; i < rhos[7].getView().extent(0); i++) {
            for (size_t j = 0; j < rhos[7].getView().extent(1); j++) {
                inputFileRho << i <<"," << j<< "," << rhos[7](i, j) << "\n";
            }
        }
        inputFileRho.close();

        //std::string precision = argv[6];

        //if (precision == "DOUBLE") {
        
        params.add("algorithm", Solver_t<double>::HOCKNEY);
        // add output type
        params.add("output_type", Solver_t<double>::SOL_AND_GRAD);

        static IpplTimings::TimerRef solverTimer = IpplTimings::getTimer("solver");
        IpplTimings::startTimer(solverTimer);

        for (size_t i = 0; i < numSlices; i++) {
            Solver_t<double> FFTsolver(Efields[i], rhos[i], params);
            // solve the Poisson equation -> rho contains the solution (phi) now
            FFTsolver.solve();
        }

        IpplTimings::stopTimer(solverTimer);

        //}
        
        // else {
            // doesnt work at the moment because the fields are init as double
            // would need to template all those types too.

        //    params.add("algorithm", Solver_t<float>::HOCKNEY);
            // add output type
        //    params.add("output_type", Solver_t<float>::SOL_AND_GRAD);

        //    Solver_t<float> FFTsolver(Efields[0], rhos[0], params);

        //    FFTsolver.solve();
        //}
               
        for (size_t slicen = 0; slicen < numSlices; slicen++) {
            auto s = std::to_string(slicen);
            std::string filename = "output_rho_" + s +".csv";

            std::ofstream outputFileRho(filename);

            // Write the view data to the output file
            outputFileRho << "i,j,val\n";
            for (size_t i = 0; i < rhos[slicen].getView().extent(0); i++) {
                for (size_t j = 0; j < rhos[slicen].getView().extent(1); j++) {
                    outputFileRho << i <<"," << j<< "," << rhos[slicen](i, j) << "\n";
                }
            }
            // Close the output file
            outputFileRho.close();
        }
        
        
        // 1D SPACE CHARGE CALCULATION START 

        // 1d solve before gather!!
        // 1) get num particles per slice from mapping - this is the longitudinal line density
        // 2) calculate the gradient of the line density wrt to the slicing axis
        // 3) compute geometery factor
        // 4) evaluate formula for E-z field

        // use mapping to deduce the number of particles per slice
        std::map<int, int> occurrences;

        // Count occurrences of each value
        auto mappingView = P->mapping.getView();

        for (size_t i = 0; i < P->getLocalNum(); i++) {
            occurrences[mappingView(i)]++;
        }

        std::vector<double> particlesPerSlice;

        for (const auto& entry : occurrences) {
            std::cerr << "Bin index: " << entry.first << ", num particles: " << entry.second << std::endl;
            particlesPerSlice.push_back(entry.second); //  this is the longitudinal line density
        }

        std::vector<double> lineDensityGradient(particlesPerSlice.size());

        for (size_t i = 1; i < particlesPerSlice.size() - 1; ++i) {
            lineDensityGradient[i] = particlesPerSlice[i + 1] - particlesPerSlice[i - 1];

        }

        // Handle edge cases manually
        lineDensityGradient[0] = particlesPerSlice[1] - particlesPerSlice[0];
        lineDensityGradient[particlesPerSlice.size() - 1] = particlesPerSlice[particlesPerSlice.size() - 1] - particlesPerSlice[particlesPerSlice.size() - 2];

        std::ofstream outputFileLamba("output_lambda.csv");
        outputFileLamba << "index,lin_den,gradient\n";
        for (size_t i = 0; i < particlesPerSlice.size(); ++i) {
            outputFileLamba << i << "," << particlesPerSlice[i] << "," << lineDensityGradient[i] << "\n";
        }
        outputFileLamba.close();

        // parameters for 1d calc. eventually g should be selected by user, ie if user selects cyclindrical vacuum chamber then the g for a cyclindrical chmaber is used
        double d = 7e-2; // half gap between parallel plates
        // radius is bunch radius, defined above
        double g = 0.67 + 4 * log(d / (Kokkos::numbers::pi_v<double> * radius)); // geometery factor for parallel plates
        double elemCharge = 1.6e-19;
        std::cerr << "g = " << g << std::endl;
        double epsilon0 = 8.8541878128e-12;

        double gamma = (3/938.272) +1; // will need to be parsed from OPAL

        std::ofstream outputFileEz("output_Ez.csv");

        // Write the view data to the output file
        outputFileEz << "index,val\n";

        std::vector<double> EzVec;
        for (size_t i = 0; i < particlesPerSlice.size(); i++) {

            double Ez = elemCharge * (- g / (4 * Kokkos::numbers::pi_v<double> * epsilon0 * gamma*gamma)) * lineDensityGradient[i];
            outputFileEz << i << "," << Ez << "\n";
            EzVec.push_back(Ez);
        }
        outputFileEz.close();

        std::ofstream outputFileEfieldSlice("output_efieldSlices.csv");
        outputFileEfieldSlice << "slice_index,Rx,Ry,Rz,Ex,Ey,Ez\n";

        // 1D SPACE CHARGE CALCULATION END

        P->gatherFromSlices(rhos, EViews);

        for (size_t i = 0; i < mappingView.extent(0); i++) {
            // assign the longitudinal value of Efield to the particles
            // the i th parting is assigned the longituninal field vlaue from the nth bin according to mapping
            P->E(i)[2] = EzVec[mappingView(i)];

            outputFileEfieldSlice << mappingView(i) << "," << P->R(i)[0] <<","<< P->R(i)[1]<< "," << P->R(i)[2] << "," << P->E(i)[0] <<","<< P->E(i)[1]<<"," << P->E(i)[2] << "\n";
        }
        outputFileEfieldSlice.close();

        IpplTimings::stopTimer(mainTimer);
        //IpplTimings::print();
        IpplTimings::print(std::string("timing.dat"));

    }
    ippl::finalize();
    return 0;
}

