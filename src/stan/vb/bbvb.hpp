#ifndef STAN__VB__BBVB__HPP
#define STAN__VB__BBVB__HPP

#include <stan/math/prim/mat/fun/Eigen.hpp>

#include <ostream>
#include <stan/services/io/write_iteration_csv.hpp>

// #include <stan/math/prim/mat/fun/log_determinant.hpp>
// #include <stan/math/prim/arr/fun/dot_self.hpp>
#include <stan/model/util.hpp>

// #include <stan/math/functions.hpp>  // I had to add these two lines beceause
// #include <stan/math/matrix.hpp>     // the unit tests wouldn't compile...

// #include <stan/math/prim/mat/err/check_square.hpp>
// #include <stan/math/prim/scal/err/check_size_match.hpp>
// #include <stan/math/prim/scal/err/check_not_nan.hpp>

#include <stan/math/prim.hpp>

#include <stan/vb/base_vb.hpp>
#include <stan/vb/vb_params_fullrank.hpp>
#include <stan/vb/vb_params_meanfield.hpp>

#include <stan/io/dump.hpp>

namespace stan {

  namespace vb {

    template <class M, class BaseRNG>
    class bbvb : public base_vb {

    public:

      bbvb(M& m,
           Eigen::VectorXd& cont_params,
           double& elbo,
           int& n_monte_carlo,
           double& eta_stepsize,
           BaseRNG& rng,
           std::ostream* output_stream,
           int& refresh,
           std::ostream* diagnostic_stream):
        base_vb(output_stream, diagnostic_stream, "bbvb"),
        model_(m),
        cont_params_(cont_params),
        elbo_(elbo),
        rng_(rng),
        n_monte_carlo_(n_monte_carlo),
        eta_stepsize_(eta_stepsize),
        refresh_(refresh) {};

      virtual ~bbvb() {};


      /**
       * FULL-RANK ELBO
       *
       * Calculates the "blackbox" Evidence Lower BOund (ELBO) by sampling
       * from the standard multivariate normal (for now), affine transform
       * the sample, and evaluating the log joint, adjusted by the entropy
       * term of the normal, which is proportional to 0.5*logdet(L^T L)
       *
       * @param   muL   mean and cholesky factor of affine transform
       * @return        evidence lower bound (elbo)
       */
      double calc_ELBO(vb_params_fullrank const& muL) {
        // static const char* function = "stan::vb::bbvb.calc_ELBO(%1%)";
        // double error_tmp(0.0);
        double elbo(0.0);
        int dim = muL.dimension();

        int elbo_n_monte_carlo(100);

        Eigen::VectorXd z_check   = Eigen::VectorXd::Zero(dim);
        Eigen::VectorXd z_tilde   = Eigen::VectorXd::Zero(dim);
        Eigen::Matrix<stan::agrad::var,Eigen::Dynamic,1> z_tilde_var(dim);

        for (int i = 0; i < elbo_n_monte_carlo; ++i) {
          // Draw from standard normal and transform to unconstrained space
          for (int d = 0; d < dim; ++d) {
            z_check(d) = stan::prob::normal_rng(0,1,rng_);
          }
          z_tilde = muL.to_unconstrained(z_check);

          // FIXME: is this the right way to do this?
          //
          // We need to call the stan::agrad::var version of log_prob
          // to get the correct proportionality and Jacobian terms
          for (int var_index = 0; var_index < dim; ++var_index) {
            z_tilde_var(var_index) = stan::agrad::var(z_tilde(var_index));
          }
          elbo += (model_.template
                   log_prob<true,true>(z_tilde_var, &std::cout)).val();
          // END of FIXME
        }
        elbo /= static_cast<double>(elbo_n_monte_carlo);

        // Entropy of normal: 0.5 * log det (L^T L) = sum(log(abs(diag(L))))
        double tmp(0.0);
        for (int d = 0; d < dim; ++d) {
          tmp = abs(muL.L_chol()(d,d));
          if (tmp != 0.0) {
            elbo += log(tmp);
          }
        }

        return elbo;
      }


      /**
       * MEAN-FIELD ELBO
       *
       * Calculates the "blackbox" Evidence Lower BOund (ELBO) by sampling
       * from the standard multivariate normal (for now), affine transform
       * the sample, and evaluating the log joint, adjusted by the entropy
       * term of the normal, which is proportional to 0.5*logdet(L^T L)
       *
       * @param   musigmatilde   mean and log-std vector of affine transform
       * @return                 evidence lower bound (elbo)
       */
      double calc_ELBO(vb_params_meanfield const& musigmatilde) {
        double elbo(0.0);
        int dim = musigmatilde.dimension();

        int elbo_n_monte_carlo(5);

        Eigen::VectorXd z_check   = Eigen::VectorXd::Zero(dim);
        Eigen::VectorXd z_tilde   = Eigen::VectorXd::Zero(dim);
        Eigen::Matrix<stan::agrad::var,Eigen::Dynamic,1> z_tilde_var(dim);

        for (int i = 0; i < elbo_n_monte_carlo; ++i) {
          // Draw from standard normal and transform to unconstrained space
          for (int d = 0; d < dim; ++d) {
            z_check(d) = stan::prob::normal_rng(0,1,rng_);
          }
          z_tilde = musigmatilde.to_unconstrained(z_check);

          // FIXME: is this the right way to do this?
          //
          // We need to call the stan::agrad::var version of log_prob
          // to get the correct proportionality and Jacobian terms
          for (int var_index = 0; var_index < dim; ++var_index) {
            z_tilde_var(var_index) = stan::agrad::var(z_tilde(var_index));
          }
          elbo += (model_.template
                   log_prob<true,true>(z_tilde_var, &std::cout)).val();
          // END of FIXME
        }
        elbo /= static_cast<double>(elbo_n_monte_carlo);

        // Entropy of normal: 0.5 * log det diag(sigma^2) = sum(log(sigma))
        //                                                = sum(sigma_tilde)
        elbo += stan::math::sum( musigmatilde.sigma_tilde() );

        return elbo;
      }


      /**
       * FULL-RANK GRADIENTS
       *
       * Calculates the "blackbox" gradient with respect to BOTH the location
       * vector (mu) and the cholesky factor of the scale matrix (L) in
       * parallel. It uses the same gradient computed from a set of Monte Carlo
       * samples
       *
       * @param muL     mean and cholesky factor of affine transform
       * @param mu_grad gradient of location vector parameter
       * @param L_grad  gradient of scale matrix parameter
       */
      void calc_combined_grad(
        vb_params_fullrank const& muL,
        Eigen::VectorXd& mu_grad,
        Eigen::MatrixXd& L_grad) {
        static const char* function = "stan::vb::bbvb.calc_combined_grad(%1%)";

        int dim       = muL.dimension();
        double tmp_lp = 0.0;

        stan::math::check_size_match(function,
                              "Dimension of mu grad vector", mu_grad.size(),
                              "Dimension of mean vector in variational q", dim);
        stan::math::check_square(function, "Scale matrix", L_grad);
        stan::math::check_size_match(function,
                              "Dimension of scale matrix", L_grad.rows(),
                              "Dimension of mean vector in variational q", dim);

        // Initialize everything to zero
        mu_grad = Eigen::VectorXd::Zero(dim);
        L_grad  = Eigen::MatrixXd::Zero(dim,dim);
        Eigen::VectorXd tmp_mu_grad = Eigen::VectorXd::Zero(dim);
        Eigen::VectorXd z_check = Eigen::VectorXd::Zero(dim);
        Eigen::VectorXd z_tilde = Eigen::VectorXd::Zero(dim);

        // Naive Monte Carlo integration
        for (int i = 0; i < n_monte_carlo_; ++i) {

          // Draw from standard normal and transform to unconstrained space
          for (int d = 0; d < dim; ++d) {
            z_check(d) = stan::prob::normal_rng(0,1,rng_);
          }
          z_tilde = muL.to_unconstrained(z_check);

          // Compute gradient step in unconstrained space
          stan::model::gradient(model_, z_tilde, tmp_lp, tmp_mu_grad,
                                &std::cout);

          // Update mu
          mu_grad += tmp_mu_grad;

          // Update L (lower triangular)
          for (int ii = 0; ii < dim; ++ii) {
            for (int jj = 0; jj <= ii; ++jj) {
              L_grad(ii,jj) += tmp_mu_grad(ii) * z_check(jj);
            }
          }

        }
        mu_grad /= static_cast<double>(n_monte_carlo_);
        L_grad  /= static_cast<double>(n_monte_carlo_);

        // Add gradient of entropy term
        L_grad.diagonal().array() += muL.L_chol().diagonal().array().inverse();
      }


      /**
       * MEAN-FIELD GRADIENTS
       *
       * Calculates the "blackbox" gradient with respect to BOTH the location
       * vector (mu) and the variance vector (sigma^2) in parallel.
       * It uses the same gradient computed from a set of Monte Carlo
       * samples
       *
       * @param musigmatilde      mean and log-std vector of affine transform
       * @param mu_grad           gradient of mean vector parameter
       * @param sigma_tilde_grad  gradient of log-std vector parameter
       */
      void calc_combined_grad(
        vb_params_meanfield const& musigmatilde,
        Eigen::VectorXd& mu_grad,
        Eigen::VectorXd& sigma_tilde_grad) {
        static const char* function = "stan::vb::bbvb.calc_combined_grad(%1%)";

        int dim       = musigmatilde.dimension();
        double tmp_lp = 0.0;

        stan::math::check_size_match(function,
                              "Dimension of mu grad vector", mu_grad.size(),
                              "Dimension of mean vector in variational q", dim);

        // Initialize everything to zero
        mu_grad          = Eigen::VectorXd::Zero(dim);
        sigma_tilde_grad = Eigen::VectorXd::Zero(dim);

        Eigen::VectorXd tmp_mu_grad = Eigen::VectorXd::Zero(dim);

        Eigen::VectorXd z_check = Eigen::VectorXd::Zero(dim);
        Eigen::VectorXd z_tilde = Eigen::VectorXd::Zero(dim);

        // Naive Monte Carlo integration
        for (int i = 0; i < n_monte_carlo_; ++i) {

          // Draw from standard normal and transform to unconstrained space
          for (int d = 0; d < dim; ++d) {
            z_check(d) = stan::prob::normal_rng(0,1,rng_);
          }
          z_tilde = musigmatilde.to_unconstrained(z_check);

          stan::math::check_not_nan(function, "z_tilde", z_tilde);

          // Compute gradient step in unconstrained space
          stan::model::gradient(model_, z_tilde, tmp_lp, tmp_mu_grad,
                                &std::cout);

          // Update mu
          mu_grad.array() = mu_grad.array() + tmp_mu_grad.array();

          // Update sigma_tilde
          sigma_tilde_grad.array() = sigma_tilde_grad.array()
            + tmp_mu_grad.array().cwiseProduct(z_check.array());

        }
        mu_grad           /= static_cast<double>(n_monte_carlo_);
        sigma_tilde_grad  /= static_cast<double>(n_monte_carlo_);

        // multiply by exp(sigma_tilde)
        sigma_tilde_grad.array() =
          sigma_tilde_grad.array().cwiseProduct(
                                      musigmatilde.sigma_tilde().array().exp());

        // Add gradient of entropy term (just equal to element-wise 1 here)
        sigma_tilde_grad.array() += 1.0;
      }


      /**
       * FULL-RANK ROBBINS-MONRO
       *
       * Runs Robbins-Monro Stochastic Gradient for some number of iterations
       *
       * @param muL            mean and cholesky factor of affine transform
       * @param tol_rel_param   relative tolerance parameter for convergence
       * @param max_iterations max number of iterations to run algorithm
       */
      void do_robbins_monro_adagrad( vb_params_fullrank& muL,
                                     double tol_rel_param,
                                     int max_iterations ) {
        Eigen::VectorXd mu_grad = Eigen::VectorXd::Zero(model_.num_params_r());
        Eigen::MatrixXd L_grad  = Eigen::MatrixXd::Zero(model_.num_params_r(),
                                                        model_.num_params_r());

        // ADAgrad parameters
        double tau = 1.0;
        Eigen::VectorXd mu_s = Eigen::VectorXd::Zero(model_.num_params_r());
        Eigen::MatrixXd L_s  = Eigen::MatrixXd::Zero(model_.num_params_r(),
                                                     model_.num_params_r());

        // RMSprop window_size
        double window_size = 100.0;
        double post_factor = 1.0 / window_size;
        double pre_factor  = 1.0 - post_factor;

        // Copy of previous parameters, for convergence check
        vb_params_fullrank muL_prev = muL;

        std::vector<double> print_vector;

        bool do_more_iterations = true;
        int iter_counter = 0;
        double delta = std::numeric_limits<double>::max();
        while (do_more_iterations) {
          muL_prev = muL;

          // Compute gradient using Monte Carlo integration
          calc_combined_grad(muL, mu_grad, L_grad);

          // Accumulate S vector for ADAgrad
          mu_s.array() += mu_grad.array().square();
          L_s.array()  += L_grad.array().square();

          // RMSprop moving average weighting
          mu_s.array() = pre_factor * mu_s.array()
                       + post_factor * mu_grad.array().square();
          L_s.array()  = pre_factor * L_s.array()
                       + post_factor * L_grad.array().square();


          // Take ADAgrad or rmsprop step
          muL.set_mu( muL.mu().array() +
            eta_stepsize_ * mu_grad.array() / (tau + mu_s.array().sqrt()) );
          muL.set_L_chol(  muL.L_chol().array()  +
            eta_stepsize_ * L_grad.array()  / (tau + L_s.array().sqrt()) );

          // Relative change in natural parameters
          delta = rel_param_decrease(muL.nat_params(),
                                     muL_prev.nat_params());
          std::cout << iter_counter << " delta = " << delta << std::endl;

          // Write elbo and parameters to "diagnostic stream"
          if (err_stream_) {
            if (iter_counter % refresh_ == 0) {
              print_vector.clear();
              print_vector.push_back(calc_ELBO(muL));
              services::io::write_iteration_csv(*err_stream_,
                                                iter_counter, print_vector);
            }
          }

          // Check for max iterations
          if (iter_counter == max_iterations) {
            do_more_iterations = false;
          }

          // Check for convergence
          if (delta < tol_rel_param) {
            do_more_iterations = false;
          }

          ++iter_counter;

        }
      }


      /**
       * MEAN-FIELD ROBBINS-MONRO
       *
       * Runs Robbins-Monro Stochastic Gradient for some number of iterations
       *
       * @param musigmatilde    mean and log-std vector of affine transform
       * @param tol_rel_param   relative tolerance parameter for convergence
       * @param max_iterations  max number of iterations to run algorithm
       */
      void do_robbins_monro_adagrad( vb_params_meanfield& musigmatilde,
                                     double tol_rel_param,
                                     int max_iterations ) {

        // Gradients
        Eigen::VectorXd mu_grad           = Eigen::VectorXd::Zero(model_.num_params_r());
        Eigen::VectorXd sigma_tilde_grad  = Eigen::VectorXd::Zero(model_.num_params_r());

        // ADAgrad parameters
        double tau = 1.0;
        Eigen::VectorXd mu_s          = Eigen::VectorXd::Zero(model_.num_params_r());
        Eigen::VectorXd sigma_tilde_s = Eigen::VectorXd::Zero(model_.num_params_r());

        // RMSprop window_size
        double window_size = 100.0;
        double post_factor = 1.0 / window_size;
        double pre_factor  = 1.0 - post_factor;

        // Copy of previous parameters, for convergence check
        vb_params_meanfield musigmatilde_prev = musigmatilde;

        // Vector for diagnostic csv writer
        std::vector<double> print_vector;

        bool do_more_iterations = true;
        int iter_counter = 0;
        double delta = std::numeric_limits<double>::max();
        while (do_more_iterations) {
          musigmatilde_prev = musigmatilde;

          // Compute gradient using Monte Carlo integration
          calc_combined_grad(musigmatilde, mu_grad, sigma_tilde_grad);

          // Accumulate S vector for ADAgrad
          mu_s.array()           += mu_grad.array().square();
          sigma_tilde_s.array()  += sigma_tilde_grad.array().square();

          // RMSprop moving average weighting
          mu_s.array() = pre_factor * mu_s.array()
                       + post_factor * mu_grad.array().square();
          sigma_tilde_s.array()  = pre_factor * sigma_tilde_s.array()
                                 + post_factor * sigma_tilde_grad.array().square();

          // Take ADAgrad or rmsprop step
          musigmatilde.set_mu(
            musigmatilde.mu().array() +
            eta_stepsize_ * mu_grad.array() / (tau + mu_s.array().sqrt())
            );
          musigmatilde.set_sigma_tilde(
            musigmatilde.sigma_tilde().array()  +
            eta_stepsize_ * sigma_tilde_grad.array()  / (tau + sigma_tilde_s.array().sqrt())
            );

          // Relative change in natural parameters
          delta = rel_param_decrease(musigmatilde.nat_params(),
                                     musigmatilde_prev.nat_params());
          std::cout << iter_counter << " delta = " << delta << std::endl;

          // Write elbo and parameters to "diagnostic stream"
          if (err_stream_) {
            if (iter_counter % refresh_ == 0) {
              print_vector.clear();
              print_vector.push_back(calc_ELBO(musigmatilde));
              services::io::write_iteration_csv(*err_stream_,
                                                iter_counter, print_vector);
            }
          }

          // Check for max iterations
          if (iter_counter == max_iterations) {
            do_more_iterations = false;
          }

          // Check for convergence
          if (delta < tol_rel_param) {
            do_more_iterations = false;
          }

          ++iter_counter;
        }
      }

      void run_fullrank(double tol_rel_param, int max_iterations) {
        std::cout
        << "This is base_vb::bbvb::run_fullrank()" << std::endl;

        // Initialize variational parameters: mu, L
        Eigen::VectorXd mu = cont_params_;
        Eigen::MatrixXd L  = Eigen::MatrixXd::Identity(model_.num_params_r(),
                                                       model_.num_params_r());
        vb_params_fullrank muL = vb_params_fullrank(mu,L);

        // Robbins Monro ADAgrad
        do_robbins_monro_adagrad(muL, tol_rel_param, max_iterations);

        cont_params_ = muL.mu();

        std::cout
        << "mu = " << std::endl
        << muL.mu() << std::endl;

        std::cout
        << "Sigma = " << std::endl
        << muL.L_chol() * muL.L_chol().transpose() << std::endl;

        // std::stringstream s;
        // stan::io::dump_writer writer(s);
        // writer.write("mu", muL.mu());
        // s << "\n";
        // writer.write("L_chol", muL.L_chol());
        // std::string written = s.str();
        // std::cout << written << std::endl;

        return;
      }

      void run_meanfield(double tol_rel_param, int max_iterations) {
        std::cout
        << "This is base_vb::bbvb::run_meanfield()" << std::endl;

        // Initialize variational parameters: mu, sigma_tilde
        Eigen::VectorXd mu           = cont_params_;
        Eigen::MatrixXd sigma_tilde  = Eigen::VectorXd::Constant(
                                                model_.num_params_r(),
                                                1.0);
        vb_params_meanfield musigmatilde = vb_params_meanfield(mu,sigma_tilde);

        // Robbins Monro ADAgrad
        do_robbins_monro_adagrad(musigmatilde, tol_rel_param, max_iterations);

        cont_params_ = musigmatilde.mu();

        std::cout
        << "mu = " << std::endl
        << musigmatilde.mu() << std::endl;

        std::cout
        << "sigma_tilde = " << std::endl
        << musigmatilde.sigma_tilde() << std::endl;

        return;
      }

      Eigen::VectorXd const& cont_params() {
        return cont_params_;
      }

      double rel_param_decrease(Eigen::VectorXd const& prev,
                                Eigen::VectorXd const& curr) const {
        return (prev - curr).norm() / std::max(prev.norm(),
                                               std::max(curr.norm(),
                                                        1.0));
      }

    protected:

      M& model_;
      Eigen::VectorXd& cont_params_;
      double elbo_;
      BaseRNG& rng_;
      int n_monte_carlo_;
      double eta_stepsize_;
      int refresh_;

    };

  } // vb

} // stan

#endif

