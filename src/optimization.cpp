#include "optimization.h"

#include "matrix_utils.h"
#include "nnls.h"

arma::mat jump_norm(arma::mat& X, const double r_const_X) {
    arma::mat norm_(X.n_rows, X.n_cols);
    norm_.fill(1.0);
    arma::mat X_trunc(X.n_rows, X.n_cols - 1);
    arma::uvec ids = arma::regspace<arma::uvec>(1, X.n_cols - 1);
    X_trunc = X.cols(ids);
    for (int k = 0; k < X.n_rows; k++) {
        double row_norm = norm(X_trunc.row(k), 2);
        for (int j = 1; j < X.n_cols; j++) {
            if (r_const_X > row_norm) {
                norm_.at(k, j) = r_const_X / row_norm;
            } else {
                norm_.at(k, j) = 1;
            }
        }
    }
    return norm_;
}

arma::uvec update_idx(const arma::mat& prev_X, const arma::mat& new_X, const double thresh) {
    arma::rowvec prev_values = find_cosine(prev_X);
    arma::rowvec new_values = find_cosine(new_X);
    arma::uvec idx2 = find(new_values >= thresh);
    arma::uvec new_idx = {};
    for (int i = 0; i < idx2.n_elem; i++) {
        if (new_values.at(idx2[i]) >= prev_values.at(idx2[i])) {
            int sz = new_idx.size();
            new_idx.resize(sz + 1);
            new_idx(sz) = idx2[i];
        }
    }
    return new_idx;
}

arma::mat hinge_der_proportions_C__(const arma::mat& H, const arma::mat& R, double precision_) {
    int m = H.n_rows;
    int n = H.n_cols;

    arma::mat TMP(n, m * m, arma::fill::zeros);

    for (int i = 0; i < m; i++) {
        for (int j = 0; j < n; j++) {
            if (H(i, j) < 0) {
                for (int k = 0; k < m; k++) {
                    TMP(j, k + i * m) = -R(k, j);
                }
            }
        }
    }

    return reshape(arma::sum(TMP, 0), m, m).t();
}

arma::mat hinge_der_basis_C__(const arma::mat& W, const arma::mat& S, double precision_) {
    int n = W.n_cols;

    arma::mat res(n, n, arma::fill::zeros);

    for (int j = 0; j < n; j++) {
        arma::vec t = W.col(j);
        res.col(j) = arma::sum(-S.cols(find(t < -precision_)), 1);
    }

    return res;
}

double hinge_C__(const arma::mat& X) {
    arma::mat X_(X.n_rows, X.n_cols, arma::fill::zeros);
    double elem_ = 0;

    for (int i = 0; i < X.n_rows; i++) {
        for (int j = 0; j < X.n_cols; j++) {
            elem_ = X(i, j);
            if (elem_ < 0) {
                X_(i, j) = -elem_;
            }
        }
    }
    return accu(X_);
}

Rcpp::List calcErrors(const arma::mat& X,
                      const arma::mat& Omega,
                      const arma::mat& D_w,
                      const arma::mat& D_h,
                      const arma::mat& SVRt,
                      const arma::mat& R,
                      const arma::mat& S,
                      const double coef_,
                      const double coef_der_X,
                      const double coef_der_Omega,
                      const double coef_hinge_H,
                      const double coef_hinge_W,
                      const double coef_pos_D_h,
                      const double coef_pos_D_w) {
    arma::mat D_w_diag = diagmat(D_w);

    double deconv_error = pow(norm(SVRt - Omega * D_w_diag * X, "fro"), 2.0);
    // don't calculate since it is time consuming, should deliver the same minimum as th new one
    // double orig_deconv_error = pow(norm(V_row - S.t() * Omega * D_w_diag * X * R, "fro"), 2);
    double lambda_error = coef_ * coef_hinge_H * hinge_C__(X * R);
    double beta_error = coef_ * coef_hinge_W * hinge_C__(S.t() * Omega);
    arma::mat A = arma::sum(R, 1);
    arma::mat B = arma::sum(S, 1);
    double D_h_error = coef_pos_D_h * pow(norm(X.t() * D_h - A, "fro"), 2);
    double D_w_error = coef_pos_D_w * pow(norm(Omega * D_w - B, "fro"), 2);
    double new_error = deconv_error + lambda_error + beta_error + D_h_error + D_w_error;

    return Rcpp::List::create(Rcpp::Named("deconv_error") = deconv_error,
                              Rcpp::Named("lambda_error") = lambda_error,
                              Rcpp::Named("beta_error") = beta_error,
                              Rcpp::Named("D_h_error") = D_h_error,
                              Rcpp::Named("D_w_error") = D_w_error,
                              Rcpp::Named("total_error") = new_error);
}

Rcpp::List derivative_stage2(const arma::mat& X,
                             const arma::mat& Omega,
                             const arma::mat& D_w,
                             const arma::mat& SVRt,
                             const arma::mat& R,
                             const arma::mat& S,
                             const double coef_der_X,
                             const double coef_der_Omega,
                             const double coef_hinge_H,
                             const double coef_hinge_W,
                             const double coef_pos_D_h,
                             const double coef_pos_D_w,
                             const int cell_types,
                             const double N,
                             const double M,
                             const int iterations,
                             const double mean_radius_X,
                             const double mean_radius_Omega,
                             const double r_const_X,
                             const double r_const_Omega,
                             const double thresh) {
    arma::mat errors_statistics(iterations, 9, arma::fill::zeros);
    arma::mat points_statistics_X(iterations, cell_types * cell_types, arma::fill::zeros);
    arma::mat points_statistics_Omega(iterations, cell_types * cell_types, arma::fill::zeros);

    arma::mat new_X = X;
    arma::mat new_Omega = Omega;
    arma::mat final_X = X;
    arma::mat final_Omega = Omega;
    arma::mat new_D_w = D_w;
    arma::mat new_D_w_x = D_w;
    arma::mat new_D_w_x_sqrt = arma::sqrt(new_D_w_x);
    arma::mat new_D_w_omega = D_w;
    arma::mat new_D_w_omega_sqrt = arma::sqrt(new_D_w_omega);
    arma::mat new_D_h = new_D_w * (N / M);
    Rcpp::Rcout << "sqrt N" << std::endl;
    Rcpp::Rcout <<  sqrt(N) << std::endl;
    Rcpp::Rcout << "sqrt M"  << std::endl;
    Rcpp::Rcout << sqrt(M) << std::endl;

    arma::vec Sigma = arma::diagvec(SVRt);
    arma::vec sqrt_Sigma = arma::sqrt(Sigma);
    arma::vec sqrt_D_w = arma::sqrt(D_w);
    Rcpp::Rcout << "sqrt Sigma" << std::endl;
    Rcpp::Rcout <<  sqrt_Sigma << std::endl;
    Rcpp::Rcout << "sqrt_D_W"  << std::endl;
    Rcpp::Rcout << sqrt_D_w << std::endl;

    Rcpp::Rcout << "Original X" << std::endl;
    Rcpp::Rcout <<  new_X << std::endl;
    Rcpp::Rcout << "Original Omega"  << std::endl;
    Rcpp::Rcout << new_Omega << std::endl;

    new_X =  arma::diagmat(new_D_w_x_sqrt) * new_X * arma::diagmat(1 / sqrt_Sigma);
    new_Omega =  arma::diagmat(1 / sqrt_Sigma) *  new_Omega * arma::diagmat(new_D_w_omega_sqrt);

    Rcpp::Rcout << "Original X tilda" << std::endl;
    Rcpp::Rcout <<  new_X << std::endl;
    Rcpp::Rcout << "Original Omega tilda"  << std::endl;
    Rcpp::Rcout << new_Omega << std::endl;


    arma::mat jump_X, jump_Omega;

    arma::vec vectorised_SVRt = arma::vectorise(SVRt);
    arma::colvec sum_rows_R = arma::sum(R, 1);
    arma::colvec sum_rows_S = arma::sum(S, 1);

    arma::mat B = join_cols(vectorised_SVRt, coef_pos_D_w * sum_rows_S);
    arma::mat C = join_cols(vectorised_SVRt, coef_pos_D_h * sum_rows_R);
    arma::mat der_X, der_Omega;


    Rcpp::Rcout << "-----iterations start----" << std::endl;
    for (int itr_ = 0; itr_ < iterations; itr_++) {
        bool has_jump_X = false;
        bool has_jump_Omega = false;
        // derivative X
        //der_X = -2 * (new_Omega.t() * (SVRt - new_Omega * new_X));
        //der_X +=  coef_hinge_H * hinge_der_proportions_C__(new_X  * R, R);

        der_X =  coef_hinge_H * hinge_der_proportions_C__(new_X  * arma::diagmat(sqrt_Sigma)  * R, R) * arma::diagmat(1 / sqrt_Sigma);
        Rcpp::Rcout << "Der_X" << std::endl;
        Rcpp::Rcout << der_X << std::endl;
        der_X = correctByNorm(der_X) * mean_radius_X;
        Rcpp::Rcout << " corrected Der_X" << std::endl;
        Rcpp::Rcout << der_X << std::endl;

        // Update X
        new_X = new_X - coef_der_X * der_X;
        // threshold for length of the new X
        Rcpp::Rcout << "now X tilda is " << std::endl;
        Rcpp::Rcout << new_X << std::endl;
        new_Omega = arma::inv(new_X);
        Rcpp::Rcout << "now Omega tilda as inv to X is" << std::endl;
        Rcpp::Rcout << new_Omega << std::endl;
        new_D_w_x_sqrt =  new_X.col(0) * sqrt_Sigma.at(0) * sqrt(N);
        new_D_w_x = arma::pow(new_D_w_x_sqrt, 2);
        Rcpp::Rcout << "based on step changed X D should be" << std::endl;
        Rcpp::Rcout <<  new_D_w_x << std::endl;
        Rcpp::Rcout << "Check that we got sqrt(N) in column" << std::endl;
        Rcpp::Rcout << (new_X.col(0) / new_D_w_x_sqrt)* sqrt_Sigma.at(0) << std::endl;

        new_D_w_omega_sqrt =  new_Omega.row(0).as_col() * sqrt_Sigma.at(0) * sqrt(M);
        new_D_w_omega = arma::pow(new_D_w_omega_sqrt, 2);
        Rcpp::Rcout << "based on inv X, Omega D should be" << std::endl;
        Rcpp::Rcout <<  new_D_w_omega << std::endl;
        Rcpp::Rcout << "Check that we got sqrt(M) in row" << std::endl;
        Rcpp::Rcout << (new_Omega.row(0).as_col() / new_D_w_omega_sqrt)* sqrt_Sigma.at(0) << std::endl;


        // derivative Omega
//        der_Omega = -2 * (SVRt - new_Omega * new_X) * new_X.t();
//        der_Omega += coef_hinge_W * hinge_der_basis_C__(S.t() * new_Omega, S);
//        der_Omega += coef_hinge_W * hinge_der_basis_C__(S.t() * new_Omega, S);
        der_Omega = coef_hinge_W * arma::diagmat(1 / sqrt_Sigma) * hinge_der_basis_C__(S.t() * arma::diagmat(sqrt_Sigma) * new_Omega, S);
        Rcpp::Rcout << "Der_Omega" << std::endl;
        Rcpp::Rcout << der_Omega << std::endl;

        der_Omega = correctByNorm(der_Omega) * mean_radius_Omega;
        Rcpp::Rcout << " corrected der_Omega" << std::endl;
        Rcpp::Rcout << der_Omega << std::endl;

        new_Omega = new_Omega - coef_der_Omega * der_Omega;
        new_X = arma::inv(new_Omega);
        Rcpp::Rcout << "now Omega tilda is " << std::endl;
        Rcpp::Rcout << new_Omega << std::endl;
        Rcpp::Rcout << "now X tilda as inv to Omega is" << std::endl;
        Rcpp::Rcout << new_X << std::endl;

       // Rcpp::Rcout << "going to get D_w from first column" << std::endl;
        new_D_w_omega_sqrt = new_Omega.row(0).as_col() * sqrt_Sigma.at(0) * sqrt(M);
        new_D_w_omega = arma::pow(new_D_w_omega_sqrt, 2);

        Rcpp::Rcout << "based on step changed Omega D should be" << std::endl;
        Rcpp::Rcout <<  new_D_w_omega << std::endl;
        new_D_w_x_sqrt =  new_X.col(0) * sqrt_Sigma.at(0) * sqrt(N);
        new_D_w_x = arma::pow(new_D_w_x_sqrt, 2);
        Rcpp::Rcout << "based on inv X, Omega D should be" << std::endl;
        Rcpp::Rcout <<  new_D_w_x << std::endl;

        Rcpp::Rcout << "Took this value as D_w" << std::endl;
        new_D_w = new_D_w_x;
//        Rcpp::Rcout << "Took first value " << std::endl;
//        Rcpp::Rcout << new_D_w_x << std::endl;
        new_D_w = arma::pow(new_D_w, 2);
        new_D_h = new_D_w * (N / M);
        arma::uword neg_props = getNegative(new_X * R);
        arma::uword neg_basis = getNegative(S.t() * new_Omega);
        double sum_ = accu(new_D_w) / M;

        // result X and omega
        final_X = arma::diagmat(1/new_D_w) * new_X * arma::diagmat(sqrt_Sigma);
        Rcpp::Rcout << "now final X is " << std::endl;
        Rcpp::Rcout << final_X << std::endl;
        final_Omega = arma::diagmat(sqrt_Sigma)* new_Omega * arma::diagmat(1/new_D_w);
        Rcpp::Rcout << "now final Omega is " << std::endl;
        Rcpp::Rcout << final_Omega << std::endl;

        Rcpp::List current_errors = calcErrors(final_X,
                                               final_Omega,
                                               new_D_w,
                                               new_D_h,
                                               SVRt,
                                               R,
                                               S,
                                               1,
                                               coef_der_X,
                                               coef_der_Omega,
                                               coef_hinge_H,
                                               coef_hinge_W,
                                               coef_pos_D_h,
                                               coef_pos_D_w);

        errors_statistics.row(itr_) = arma::rowvec{current_errors["deconv_error"],
                                                   current_errors["lambda_error"],
                                                   current_errors["beta_error"],
                                                   current_errors["D_h_error"],
                                                   current_errors["D_w_error"],
                                                   current_errors["total_error"],
                                                   neg_props,
                                                   neg_basis,
                                                   sum_};


        points_statistics_X.row(itr_) = final_X.as_row();
        points_statistics_Omega.row(itr_) = final_Omega.as_row();
    }


    return Rcpp::List::create(Rcpp::Named("new_X") = final_X,
                              Rcpp::Named("new_Omega") = final_Omega,
                              Rcpp::Named("new_D_w") = new_D_w,
                              Rcpp::Named("new_D_h") = new_D_h,
                              Rcpp::Named("errors_statistics") = errors_statistics,
                              Rcpp::Named("points_statistics_X") = points_statistics_X,
                              Rcpp::Named("points_statistics_Omega") = points_statistics_Omega);
}
