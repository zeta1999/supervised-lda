#ifndef _LDA_HPP_
#define _LDA_HPP_


#include <condition_variable>
#include <list>
#include <memory>
#include <mutex>
#include <vector>
#include <thread>
#include <tuple>

#include <Eigen/Core>

#include "ldaplusplus/events/Events.hpp"
#include "ldaplusplus/em/IEStep.hpp"
#include "ldaplusplus/em/IMStep.hpp"
#include "ldaplusplus/Parameters.hpp"

namespace ldaplusplus {


/**
 * LDA contains the logic of using an expectation step, a maximization step and
 * some model parameters to train and make use of an LDA model.
 *
 * 1. It is agnostic of the underlying implementations it uses and thus allows
 *    for experimentation through a common facade.
 * 2. It uses multiple threads to compute the time consuming expectation step.
 * 3. It aggregates all the events and redispatches them on the same thread
 *    through a single event dispatcher.
 * 4. It provides a very simple interface (borrowed from scikit-learn)
 */
template <typename Scalar>
class LDA
{
    typedef Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic> MatrixX;
    typedef Eigen::Matrix<Scalar, Eigen::Dynamic, 1> VectorX;

    public:
        /**
         * Create an LDA with the given model parameters, expectation and
         * maximization steps default iterations and worker threads.
         *
         * @param model_parameters A pointer to a struct containing the model
         *                         parameters (for instance ModelParameters and
         *                         SupervisedModelParameters)
         * @param e_step           A pointer to an expectation step
         *                         implementation
         * @param m_step           A pointer to a maximization step
         *                         implementation
         * @param iterations       The number of epochs to run when using
         *                         LDA::fit
         * @param workers          The number of worker threads to create for
         *                         computing the expectation step
         */
        LDA(
            std::shared_ptr<parameters::Parameters> model_parameters,
            std::shared_ptr<em::IEStep<Scalar> > e_step,
            std::shared_ptr<em::IMStep<Scalar> > m_step,
            size_t iterations = 20,
            size_t workers = 1
        );

        /**
         * Create a move constructor that doesn't try to copy or move mutexes.
         */
        LDA(LDA &&lda);

        /**
         * Compute a supervised topic model for word counts X and classes y.
         *
         * Perform as many EM iterations as configured and stop when reaching
         * max_iter_ or any other stopping criterion.
         *
         * An EigenClassificationCorpus will be created from the passed
         * parameters.
         *
         * @param X The word counts in column-major order
         * @param y The classes as integers
         */
        void fit(const Eigen::MatrixXi &X, const Eigen::VectorXi &y);

        /**
         * Perform a single EM iteration.
         *
         * An EigenClassificationCorpus will be created from the passed
         * parameters.
         *
         * @param X The word counts in column-major order
         * @param y The classes as integers
         */
        void partial_fit(const Eigen::MatrixXi &X, const Eigen::VectorXi &y);

        /**
         * Perform a single EM iteration.
         *
         * @param corpus The implementation of Corpus that contains the
         *               observed variables.
         */
        void partial_fit(std::shared_ptr<corpus::Corpus> corpus);

        /**
         * Run the expectation step and return the topic mixtures for the
         * documents defined by the word counts X.
         *
         * @param  X The word counts in column-major order
         * @return The variational parameter \f$\gamma\f$ for every document
         *         that approximates the count of words generated by each topic
         */
        MatrixX transform(const Eigen::MatrixXi &X);

        /**
         * Treat the SupervisedModelParameters::eta as a linear model and
         * compute the distances from the planes of the documents in the topic
         * space.
         *
         * Use LDA::transform to obtain the \f$\gamma\f$ for every document and
         * then assume that the \f$\eta\f$ parameters of the
         * SupervisedModelParameters are a linear model. Compute the dot
         * product between the normal vectors and the normalized topic mixtures
         * for each document. The more positive the value for a given class the
         * more confident is the model that a document belongs in this class.
         *
         * @param  X The word counts in column-major order
         * @return A matrix of class scores (positive => confident) for each
         *         document
         */
        MatrixX decision_function(const Eigen::MatrixXi &X);

        /**
         * Use the model to predict the class indexes for the word counts X.
         *
         * Use LDA::decision_function to get class scores and then compute the
         * *argmax* for every document.
         *
         * @param  X The word counts in column-major order
         * @return A matrix of class indexes (the predicted class for each
         *         document)
         */
        Eigen::VectorXi predict(const Eigen::MatrixXi &X);

        /**
         * Return both the class predictions and the transformed data using a
         * single LDA expectation step.
         *
         * @param  X The word counts in column-major order
         * @return A tuple containing a matrix with the \f$\gamma\f$
         *         variational parameters for every document and a vector
         *         containing the class index predictions for every document.
         */
        std::tuple<MatrixX, Eigen::VectorXi> transform_predict(const Eigen::MatrixXi &X);

        /**
         * Get the event dispatcher for this LDA instance.
         */
        std::shared_ptr<events::IEventDispatcher> get_event_dispatcher() {
            return event_dispatcher_;
        }

        /**
         * Get a constant reference to the model's parameters.
         */
        const std::shared_ptr<parameters::Parameters> model_parameters() {
            return model_parameters_;
        }

    protected:
        /**
         * Generate a Corpus from a pair of X, y matrices
         */
        std::shared_ptr<corpus::Corpus> get_corpus(
            const Eigen::MatrixXi &X,
            const Eigen::VectorXi &y
        );

        /**
         * Generate a Corpus from just the word count matrix.
         */
        std::shared_ptr<corpus::Corpus> get_corpus(const Eigen::MatrixXi &X);

        /**
         * Create a worker thread pool.
         */
        void create_worker_pool();

        /**
         * Destroy the worker thread pool
         */
        void destroy_worker_pool();

        /**
         * Forward the events generated in the worker threads to this event
         * dispatcher in this thread.
         */
        void process_worker_events() {
            std::static_pointer_cast<events::ThreadSafeEventDispatcher>(
                event_dispatcher_
            )->process_events();
        }

        /**
         * Extract the variational parameters and the document index from the
         * worker queue.
         */
        std::tuple<std::shared_ptr<parameters::Parameters>, size_t> extract_vp_from_queue();

        /**
         * A doc_e_step worker thread.
         */
        void doc_e_step_worker();

        /**
         * Implement the decision function using already transformed data.
         * Topic representations instead of BOW.
         *
         * @param X The \f$\gamma\f$ variational parameter for each document in
         *          a column-major ordered matrix.
         * @return A matrix of class scores (positive => confident) for each
         *         document
         */
        MatrixX decision_function(const MatrixX &X);

        /**
         * Transform the decision function to class predictions.
         */
        Eigen::VectorXi predict(const MatrixX &scores);


    private:
        /**
         * Pass the event dispatcher down to the implementations so that they
         * can communicate with the outside world.
         */
        void set_up_event_dispatcher();

        // The model parameters
        std::shared_ptr<parameters::Parameters> model_parameters_;

        // The LDA implementation
        std::shared_ptr<em::IEStep<Scalar> > e_step_;
        std::shared_ptr<em::IMStep<Scalar> > m_step_;

        // Member variables that affect the behaviour of fit
        size_t iterations_;

        // The thread related member variables
        std::vector<std::thread> workers_;
        std::mutex queue_in_mutex_;
        std::list<std::tuple<std::shared_ptr<corpus::Corpus>, size_t> > queue_in_;
        std::mutex queue_out_mutex_;
        std::condition_variable queue_out_cv_;
        std::list<std::tuple<std::shared_ptr<parameters::Parameters>, size_t> > queue_out_;

        // An event dispatcher that we will use to communicate with the
        // external components
        std::shared_ptr<events::IEventDispatcher> event_dispatcher_;
};


}
#endif  // _LDA_HPP_