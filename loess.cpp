 /*
  Copyright (c) 2016, Bart Vermeulen
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:

      * Redistributions of source code must retain the above copyright
        notice, this list of conditions and the following disclaimer.
      * Redistributions in binary form must reproduce the above copyright
        notice, this list of conditions and the following disclaimer in
        the documentation and/or other materials provided with the distribution

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
  POSSIBILITY OF SUCH DAMAGE.
*/

#include "loess.h"

#include <CGAL/basic.h>
#include <CGAL/Search_traits.h>
#include <CGAL/Orthogonal_incremental_neighbor_search.h>
#include <CGAL/iterator.h>
#include <math.h>
#include <algorithm>
#include <iterator>
#include <thread>
#include <functional>
#include <future>
#include <chrono>
#include <exception>

using namespace std;

extern void main_();

typedef CGAL::Search_traits<double, Point, Point::Cit, Construct_coord_iterator> Traits; // search traits
typedef CGAL::Orthogonal_incremental_neighbor_search<Traits> K_inc_neighbor_search; // incremental searcher
typedef K_inc_neighbor_search::Tree Tree; // The tree
typedef K_inc_neighbor_search::iterator Piterator; //The search iterator
typedef K_inc_neighbor_search::Point_with_transformed_distance P_with_dist; //Return type of searcher
struct Point_rw_not_zero{
	bool operator() (const Piterator& it){
		return it->first.rw()==0; //functor to exclude points with rw == 0
	}
};
typedef CGAL::Filter_iterator< Piterator, Point_rw_not_zero > P_pos_rw_it; // construct filtered iterator


// internal functions
void loess(vector< Point > &,const vector< Point >&, vector< double > &, size_t, size_t, size_t, size_t);

typedef std::vector< P_with_dist >::const_iterator regpoints_iit;
typedef std::back_insert_iterator< std::vector< double > > weights_oit;
void triCube(regpoints_iit, regpoints_iit, weights_oit);

void biCube(Tree const & tree, const vector< double > &);

void localFit( Tree const &, vector< Point > const & qp,size_t q, vector< double > &, size_t, size_t, double &);
double median(vector< double >);


MatrixX1d loess(Eigen::MatrixXd x, Eigen::Matrix<double, Eigen::Dynamic, 1> v, Eigen::MatrixXd xi, double span, size_t niter, size_t order, size_t nthreads)
{
	size_t nin = x.rows();
	size_t nd = x.cols();
	size_t nout = xi.rows();

	// check arguments
	if(nin != v.rows())
	{
		throw std::invalid_argument("Second input (values) should have same number of rows as first input (locations)");
	}
	if(nd != xi.cols())
	{
		throw std::invalid_argument("Third input(query points) should have same number of columns as first input (locations)");
	}
	if (order != 1 && order != 2)
	{
		throw std::invalid_argument("Sixth argument should be equal to one or two");
	}


	if (nthreads == 0)
	{
		nthreads = std::thread::hardware_concurrency();
	}

	// Make output variable
	MatrixX1d vi(nout);

	// Transfer vars into vectors
	vector< Point > inpoints, outpoints;
	vector< double > valsout;
	//inpoints.resize(nin,Point(nd,0));
	outpoints.resize(nout, Point(nd, 0));
	valsout.resize(nout, std::numeric_limits<double>::quiet_NaN());
	for (size_t cin = 0; cin < nin; cin++) {
		if (!std::isfinite(v(cin)))
			continue;
		Point tmpPoint(nd, 0);
		tmpPoint.val(v(cin));
		bool point_is_finite = true;
		for (size_t cd = 0; (cd < nd) & point_is_finite; cd++) {
			if (!std::isfinite(x(cd * nin + cin))) {
				point_is_finite = false;
				continue;
			}
			tmpPoint[cd] = (x(cd * nin + cin));
		}
		if (point_is_finite)
			inpoints.push_back(tmpPoint);
	}
	for (size_t co = 0; co < nout; co++) {
		for (size_t cd = 0; cd < nd; cd++) {
			outpoints[co][cd] = xi(cd * nout + co);
		}
	}

	//Make span
	size_t q;
	if (span > 1)
		q = static_cast<size_t>(std::floor(span));
	else
		q = static_cast<size_t>(std::floor(span * static_cast<double> (nin)));

	q = std::max(static_cast<size_t>(3), std::min(nin, q));

	//Perform computation
	loess(inpoints, outpoints, valsout, q, niter, order, nthreads);

	// Copy output
	for (size_t co = 0; co < nout; co++) {
		vi[co] = valsout[co];
	}

	return vi;
}

void loess(vector< Point > & inpoints,const vector < Point > & outpoints, vector <double> & valsout, size_t q, size_t niter, size_t order, size_t nthreads){
	// Build search tree
	Tree tree(inpoints.begin(), inpoints.end());

	// Perform regression on input data
	double prog_lf(0);
	vector<double> vals_reg=vector<double>(inpoints.size(),0); // holds regression results at input locations
	double frac_riter = double(inpoints.size())/double(inpoints.size()*niter+outpoints.size());
	double frac_interp = double(outpoints.size())/double(inpoints.size()*niter+outpoints.size());
	double prog(0);
	std::cout << std::fixed << std::setw(10) << std::setprecision(2);
	for (size_t citer=0; citer < niter; citer++) {// robust iterations
	    prog_lf=0;
	    std::future< void > f = std::async(std::launch::async, localFit, std::ref(tree), std::ref(inpoints),q,std::ref(vals_reg), order, nthreads, std::ref(prog_lf));
		std::future_status status;
	    do {
	    	status = f.wait_for(std::chrono::seconds(1));
	    	prog = (double(citer) + prog_lf) * frac_riter;
            printf("\r%10.2f%%", prog*100);
	    	//std::cout << "\r" << prog*100 << "%" << std::flush;
	    } while (status != std::future_status::ready);
	    biCube(tree,vals_reg); // Compute robust weights (This is not included in computation of progress)
	}

	// Perform regression on output points
	prog_lf = 0;
	std::future< void > f = std::async(std::launch::async, localFit, std::ref(tree), std::ref(outpoints),q,std::ref(valsout),order, nthreads, std::ref(prog_lf));
	std::future_status status;
    do {
    	status = f.wait_for(std::chrono::seconds(1));
    	prog = prog_lf * frac_interp + double(niter) * frac_riter;
        printf("\r%10.2f%%", prog*100);
    	//std::cout << "\r" << prog*100 << "%" << std::flush;
    } while (status != std::future_status::ready);
    std::cout << "\rDone.      " << std::endl;
}


void triCube(regpoints_iit pd_begin, regpoints_iit pd_end,  weights_oit w_it) {
	// Computes regression weights
	double arg=0;
	for (auto pd_it=pd_begin; pd_it != pd_end; pd_it++){
		arg=pd_it->second / (pd_end-1)->second;
		*w_it = pd_it->first.rw() * ( (arg < 1) ? pow(1 - pow(arg,1.5),3) : 0);
		++w_it;
	}
}

void biCube(Tree const & tree, const vector<double> & vals_reg ){
	// biCube function for the computation of robust weights from residuals of the fit
	double arg=0;
	// calculate residuals
	vector<double> res=vector<double>(tree.size(),0);
	for (std::size_t cp=0; cp<static_cast<std::size_t>(tree.size()); cp++)
		res[cp]=abs((tree.begin()+cp)->val()-vals_reg[cp]);
	double sixmres=6*median(res);
	for (std::size_t cp=0; cp<static_cast<std::size_t>(tree.size()); cp++){
		arg=res[cp]/sixmres;
		const_cast<Point*> (&(*(tree.begin()+cp)))->rw( (arg < 1) ? pow(1-pow(arg,2),2) : 0 ); //const_cast shouldn't do any harm here
	}
}

void parFit(const Tree & tree, vector< Point >::const_iterator qp_begin, vector< Point >::const_iterator qp_end, vector< double >::iterator val_beg, size_t q, size_t n, size_t order, double & prog){
// Performs the actual local regression
// Input:
// 		Tree:      the spatial search tree (not modified)
// 		qp_begin:  begin iterator of query points (not modified)
// 		qp_end:	   end iterator of query points (not modified)
// 		val_begin: begin iterator of values (modified to hold output of regression)
// 		q:         number of points for regression (not modified)
// 		n:		   number of terms in regression (not modified)
// 		order:	   order of regression (not modified)
// 		prog:	   to keep track of progress, between 0 and 1 (modified)
	// Initialize variable
	size_t ndims=qp_begin->dims();      // number of dimensions

	
	// Search for N-nearest neighbors and perform regression
	for (auto qp_it = qp_begin; qp_it != qp_end; qp_it++){ // loop over all query points
		bool point_is_finite=true;
		for (auto c_it = qp_it->begin(); c_it != qp_it->end(); c_it++)
			if (!std::isfinite(*c_it))
				point_is_finite=false;
		if (!point_is_finite){
			++val_beg;
			continue;
		}
		K_inc_neighbor_search ins (tree,*qp_it); //Create incremental searcher
		P_pos_rw_it it(ins.end(), Point_rw_not_zero(), ins.begin()), end(ins.end(), Point_rw_not_zero()); //filtered iterator to exclude non-finite values and nan-values
		vector< P_with_dist > regpoints; // holds nearest neighbor search results

		// Copy N-nearest points to current query point
		for (size_t cc=0; cc < q && it!=end; cc++){
			regpoints.push_back(*it);// store filtered nearest neighbors
			it++;
		}
		
		if (regpoints.size() < n){
			++val_beg;
			continue;
		}		
		// Compute weights for regression
		vector< double > w; 				// hold regression weights
		triCube(regpoints.cbegin(), regpoints.cend(), std::back_inserter(w));
		
		// Eigen matrices for regression
		Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic> A(regpoints.size(),n); // Regression matrix
		Eigen::Matrix<double, Eigen::Dynamic, 1> x(n,1);		      // Regression result vector
		Eigen::Matrix<double, Eigen::Dynamic, 1> b(regpoints.size(),1);              // Known values for regression
		// Fill EIGEN matrices and perform regression
		for (size_t cc=0; cc < regpoints.size(); cc++) { // loop over points to regress
			A(cc,0)=w[cc]; // Term 1: Intercept, i.e. 1*weight (zero-th order)
			b(cc,0)=regpoints[cc].first.val()*w[cc]; // Known value, i.e. value * weight
			for (size_t cd=0; cd < ndims ; cd++)  // Regression terms (1st order)
				A(cc,cd+1)=w[cc]*(regpoints[cc].first[cd]-(*qp_it)[cd]); // Terms are weighed centered coordinates
			if (order==2){ // Quadratic terms (2nd order), i.e. all cross products
				size_t cpos=ndims+1; // Term number in matrix (column number)
				for (size_t cd1=0; cd1<ndims; cd1++) // Loop over dimension
					for (size_t cd2=cd1; cd2 < ndims; cd2++ ) // Loop from outer loop dimension to number of dimensions
						A(cc, cpos++)=w[cc] * (regpoints[cc].first[cd1]-(*qp_it)[cd1]) * (regpoints[cc].first[cd2]-(*qp_it)[cd2]); // Assign cross-products (centered and weighed)
			}
		}

		//x=A.fullPivLu().solve(b);//perform regression
		 // x=A.fullPivHouseholderQr().solve(b);//perform regression
		//x=A.colPivHouseholderQr().solve(b);//perform regression
		//x=A.householderQr().solve(b);
		x=A.jacobiSvd(Eigen::ComputeThinU | Eigen::ComputeThinV).solve(b); // Perform least squares regression
		* val_beg = x(0,0); // store result
		++val_beg;	// Increase pointer to output values
		prog = double(std::distance(qp_begin,qp_it))/double(std::distance(qp_begin,qp_end)); // keep track of progress
	}
	prog = 1;
}

void localFit(const Tree & tree, const vector< Point > & qp, size_t q, vector<double> & val, size_t order, size_t nthreads, double & prog) {
	// This function controls the local fitting by calling the function parFit in separate computational threads.
	// Inputs are:
	// 	tree:     The Spatial search tree (not modified)
	// 	qp:       Query points (not modified)
	// 	q:	      Number of points in regression (not modified)
	// 	val:      Estimated function value at query points (modified)
	// 	order:    Order of the regression (not modified)
	// 	nthreads: Number of computational threads (not modified)
	// 	prog:	  Keeps track of progress (modified)

	// Compute number of input points and dimensions
	size_t nin=qp.size();
	size_t ndims=qp.begin()->dims();

	// Compute number of terms in regression
	// 	Linear terms
	size_t n=ndims+1; // number of dimensions plus one
	//  Quadratic terms
	if (order == 2)
		for (size_t cd=1; cd < ndims + 1; cd++)
			n += cd; // cross-products (1 + 2 + ... + ndims)

	size_t usedthreads = nthreads>nin ? nin : nthreads; // Reduce the number of threads when there are less query points than threads
	// Compute number of points in each each computational thread
	vector< size_t > n_in_thread(usedthreads,nin/usedthreads); // Number of threads is equal to the integral division of number of points and threads
	for (size_t cr=0; cr < nin % usedthreads; cr++)         // Remaining point (modulo) are spread over the threads
		n_in_thread[cr]++;

	// Start computation in threads asynchronously
	vector<double> prog_th(usedthreads,0); 			 // Vector holding progress (between 0 and 1) of each computation thread
	std::vector< std::future < void > > all_futures; // Vector with one feature per thread
	size_t total=0; 								 // Keeps track of points passed to previous threads
	for (size_t cth = 0; cth<usedthreads; cth++){       // Loop to launch computations asynchronously
		all_futures.push_back( std::async( std::launch::async, parFit, std::ref(tree), qp.begin()+total, qp.begin()+total+n_in_thread[cth], val.begin()+total, q, n ,order, std::ref(prog_th[cth]) ) ); // Start thread
		total += n_in_thread[cth];	// Keep track of points already passed to the function
	}

	// Check status of threads every second
	std::future_status status;
	for (size_t cth = 0; cth < usedthreads; cth++){ // Check status of each thread
		do {
			status = all_futures[cth].wait_for(std::chrono::seconds(1)); // Wait for 1 second, or for thread ending (whichever comes first)
			prog = std::accumulate(prog_th.begin(),prog_th.end(),0.0)/double(usedthreads); // Compute current progress of threads
		} while (status != std::future_status::ready); // Repeat loop until thread ended successfully
	}

} // end of localFit



double median(vector<double> vec)
{
	// Computes approxiamte median of elements in a vector
	//  Input:
	//   vec: Vector with values to compute the median.
	//        The vector is copied since it should not be modified
	vector<double>::size_type mid(vec.size()/2); // compute half the size of the matrix (rounded to zero)
	nth_element (vec.begin(), vec.begin()+mid, vec.end()); // Find median element
	return vec[mid]; // Return median element
}

