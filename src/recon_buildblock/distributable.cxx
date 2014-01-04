/*
    Copyright (C) 2000 PARAPET partners
    Copyright (C) 2000 - 2011, Hammersmith Imanet Ltd
    Copyright (C) 2013-2014, University College London
    This file is part of STIR.

    This file is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.

    This file is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    See STIR/LICENSE.txt for details
*/
/*!

  \file
  \ingroup distributable

  \brief Implementation of stir::distributable_computation() and related functions

  \author Kris Thielemans  
  \author Alexey Zverovich 
  \author Matthew Jacobson
  \author PARAPET project
  \author Tobias Beisel
*/
/* Modification history:
   KT 30/05/2002
   get rid of dependence on specific symmetries (i.e. views up to 45 degrees)
   
   Tobias Beisel 16/06/2007
   added MPI support
   added function for Cached MPI support

   Kris Thielemans Feb 2011
   move most ugly things into separate functions
   added normalisation/mult_viewgrams support
   */
#include "stir/shared_ptr.h"
#include "stir/recon_buildblock/distributable.h"
#include "stir/RelatedViewgrams.h"
#include "stir/ProjData.h"
#include "stir/ExamInfo.h"
#include "stir/DiscretisedDensity.h"
#include "stir/ViewSegmentNumbers.h"
#include "stir/CPUTimer.h"
#include "stir/recon_buildblock/ForwardProjectorByBin.h"
#include "stir/recon_buildblock/BackProjectorByBin.h"
#include "stir/recon_buildblock/BinNormalisation.h"
#include "stir/is_null_ptr.h"
#include "stir/recon_buildblock/PoissonLogLikelihoodWithLinearModelForMeanAndProjData.h" // needed for RPC functions

#ifdef STIR_MPI
#include "stir/recon_buildblock/distributableMPICacheEnabled.h"
#include "stir/recon_buildblock/distributed_functions.h"
#include "stir/recon_buildblock/distributed_test_functions.h"
#endif

#ifndef STIR_NO_NAMESPACES
using std::cerr;
using std::endl;
#endif

START_NAMESPACE_STIR

/* WARNING: the sequence of steps here has to match what is on the receiving end 
   in DistributedWorker */
void setup_distributable_computation(
                                     const shared_ptr<ProjectorByBinPair>& proj_pair_sptr,
                                     const shared_ptr<ExamInfo>& exam_info_sptr,
                                     const ProjDataInfo * const proj_data_info_ptr,
                                     const shared_ptr<DiscretisedDensity<3,float> >& target_sptr,
                                     const bool zero_seg0_end_planes,
                                     const bool distributed_cache_enabled)
{
#ifdef STIR_MPI
  distributed::first_iteration = true;
         
  //broadcast type of computation (currently only 1 available)
  distributed::send_int_value(task_setup_distributable_computation, -1);

  //broadcast zero_seg0_end_planes
  distributed::send_bool_value(zero_seg0_end_planes, -1, -1);
        
  //broadcast target_sptr
  distributed::send_image_parameters(target_sptr.get(), -1, -1);
  distributed::send_image_estimate(target_sptr.get(), -1);
        
  //sending Data_info
  distributed::send_exam_and_proj_data_info(*exam_info_sptr, *proj_data_info_ptr, -1);

  //send projector pair
  distributed::send_projectors(proj_pair_sptr, -1);

  //send configuration values for distributed computation
  int configurations[4];
  configurations[0]=distributed::test?1:0;
  configurations[1]=distributed::test_send_receive_times?1:0;
  configurations[2]=distributed::rpc_time?1:0;
  configurations[3]=distributed_cache_enabled?1:0;
  distributed::send_int_values(configurations, 4, distributed::STIR_MPI_CONF_TAG, -1);
        
  distributed::send_double_values(&distributed::min_threshold, 1, distributed::STIR_MPI_CONF_TAG, -1);
                
#ifndef NDEBUG
  //test sending parameter_info
  if (distributed::test) 
    distributed::test_parameter_info_master(proj_pair_sptr->stir::ParsingObject::parameter_info(), 1, "projector_pair_ptr");
#endif

#endif // STIR_MPI
}

void end_distributable_computation()
{
#ifdef STIR_MPI
  int my_rank;  
  MPI_Comm_rank(MPI_COMM_WORLD, &my_rank) ; /*Gets the rank of the Processor*/   
  if (my_rank==0)
    {
      //broadcast end of processing notification
      distributed::send_int_value(task_stop_processing, -1);
    }
#endif

}

template <class ViewgramsPtr>
static void
zero_end_sinograms(ViewgramsPtr viewgrams_ptr)
{
  if (!is_null_ptr(viewgrams_ptr))
    {
      const int min_ax_pos_num = viewgrams_ptr->get_min_axial_pos_num();
      const int max_ax_pos_num = viewgrams_ptr->get_max_axial_pos_num();
      for (RelatedViewgrams<float>::iterator r_viewgrams_iter = viewgrams_ptr->begin();
           r_viewgrams_iter != viewgrams_ptr->end();
           ++r_viewgrams_iter)
        {
          (*r_viewgrams_iter)[min_ax_pos_num].fill(0);
          (*r_viewgrams_iter)[max_ax_pos_num].fill(0);
        }
    }
}


static
void get_viewgrams(shared_ptr<RelatedViewgrams<float> >& y,
                   shared_ptr<RelatedViewgrams<float> >& additive_binwise_correction_viewgrams,
                   shared_ptr<RelatedViewgrams<float> >& mult_viewgrams_sptr,
                   const shared_ptr<ProjData>& proj_dat_ptr, 
                   const bool read_from_proj_dat,
                   const bool zero_seg0_end_planes,
                   const shared_ptr<ProjData>& binwise_correction,
                   const shared_ptr<BinNormalisation>& normalisation_sptr,
                   const double start_time_of_frame,
                   const double end_time_of_frame,
                   const shared_ptr<DataSymmetriesForViewSegmentNumbers>& symmetries_ptr,
                   const ViewSegmentNumbers& view_segment_num
                   )
{
  if (!is_null_ptr(binwise_correction)) 
    {
#if !defined(_MSC_VER) || _MSC_VER>1300
      additive_binwise_correction_viewgrams.reset(
        new RelatedViewgrams<float>
        (binwise_correction->get_related_viewgrams(view_segment_num, symmetries_ptr)));
#else
      RelatedViewgrams<float> tmp(binwise_correction->
                                  get_related_viewgrams(view_segment_num, symmetries_ptr));
      additive_binwise_correction_viewgrams.reset(new RelatedViewgrams<float>(tmp));
#endif      
    }
                        
  if (read_from_proj_dat)
    {
#if !defined(_MSC_VER) || _MSC_VER>1300
      y.reset(new RelatedViewgrams<float>
	      (proj_dat_ptr->get_related_viewgrams(view_segment_num, symmetries_ptr)));
#else
      // workaround VC++ 6.0 bug
      RelatedViewgrams<float> tmp(proj_dat_ptr->
                                  get_related_viewgrams(view_segment_num, symmetries_ptr));
      y.reset(new RelatedViewgrams<float>(tmp));
#endif        
    }
  else
    {
      y.reset(new RelatedViewgrams<float>
	      (proj_dat_ptr->get_empty_related_viewgrams(view_segment_num, symmetries_ptr)));
    }

  // multiplicative correction
  if (!is_null_ptr(normalisation_sptr) && !normalisation_sptr->is_trivial())
    {
      mult_viewgrams_sptr.reset(
				new RelatedViewgrams<float>(proj_dat_ptr->get_empty_related_viewgrams(view_segment_num, symmetries_ptr)));
      mult_viewgrams_sptr->fill(1.F);
      normalisation_sptr->undo(*mult_viewgrams_sptr,start_time_of_frame,end_time_of_frame);
    }
                        
  if (view_segment_num.segment_num()==0 && zero_seg0_end_planes)
    {
      zero_end_sinograms(y);
      zero_end_sinograms(additive_binwise_correction_viewgrams);
      zero_end_sinograms(mult_viewgrams_sptr);
    }
}

#ifdef STIR_MPI
void send_viewgrams(const shared_ptr<RelatedViewgrams<float> >& y,
                    const shared_ptr<RelatedViewgrams<float> >& additive_binwise_correction_viewgrams,
                    const shared_ptr<RelatedViewgrams<float> >& mult_viewgrams_sptr,
                    const int next_receiver)
{
  distributed::send_view_segment_numbers( y->get_basic_view_segment_num(), NEW_VIEWGRAM_TAG, next_receiver);

#ifndef NDEBUG
  //test sending related viegrams
  shared_ptr<DataSymmetriesForViewSegmentNumbers> 
    symmetries_sptr(y->get_symmetries_ptr()->clone());
  if (distributed::test && distributed::first_iteration==true && next_receiver==1) 
    distributed::test_related_viewgrams_master(y->get_proj_data_info_ptr()->create_shared_clone(), 
                                               symmetries_sptr, y.get(), next_receiver);
#endif

  //TODO: this could also be done by using MPI_Probe at the slave to find out what to recieve next
  if (is_null_ptr(additive_binwise_correction_viewgrams))
    {   //tell slaves that recieving additive_binwise_correction_viewgrams is not needed
      distributed::send_bool_value(false, BINWISE_CORRECTION_TAG, next_receiver);
    }
  else
    {
      //tell slaves to receive additive_binwise_correction_viewgrams
      distributed::send_bool_value(true, BINWISE_CORRECTION_TAG, next_receiver);
      distributed::send_related_viewgrams(additive_binwise_correction_viewgrams.get(), next_receiver);
    }
  if (is_null_ptr(mult_viewgrams_sptr))
    {
      //tell slaves that recieving mult_viewgrams is not needed
      distributed::send_bool_value(false, BINWISE_MULT_TAG, next_receiver);
    }
  else
    {
      //tell slaves to receive mult_viewgrams
      distributed::send_bool_value(true, BINWISE_MULT_TAG, next_receiver);
      distributed::send_related_viewgrams(mult_viewgrams_sptr.get(), next_receiver);
    }
  // send y
  distributed::send_related_viewgrams(y.get(), next_receiver);
}
#endif

void distributable_computation(
                               const shared_ptr<ForwardProjectorByBin>& forward_projector_ptr,
                               const shared_ptr<BackProjectorByBin>& back_projector_ptr,
                               const shared_ptr<DataSymmetriesForViewSegmentNumbers>& symmetries_ptr,
                               DiscretisedDensity<3,float>* output_image_ptr,
                               const DiscretisedDensity<3,float>* input_image_ptr,
                               const shared_ptr<ProjData>& proj_dat_ptr, 
                               const bool read_from_proj_dat,
                               int subset_num, int num_subsets,
                               int min_segment_num, int max_segment_num,
                               bool zero_seg0_end_planes,
                               double* log_likelihood_ptr,
                               const shared_ptr<ProjData>& binwise_correction,
                               const shared_ptr<BinNormalisation> normalisation_sptr,
                               const double start_time_of_frame,
                               const double end_time_of_frame,
                               RPC_process_related_viewgrams_type * RPC_process_related_viewgrams,
                               DistributedCachingInformation* caching_info_ptr)

{
#ifdef STIR_MPI 
  // TODO need to differentiate depending on RPC_process_related_viewgrams
  int task_id;
  if (RPC_process_related_viewgrams == &RPC_process_related_viewgrams_accumulate_loglikelihood)
    task_id=task_do_distributable_loglikelihood_computation;
  else if (RPC_process_related_viewgrams == &RPC_process_related_viewgrams_gradient)
    task_id=task_do_distributable_gradient_computation;
      /* else if (RPC_process_related_viewgrams == &
	case 
	task_id=task_do_distributable_sensitivity_computation;break;
      */
  else
    {
      error("distributable_computation: unknown RPC task");
    }

  distributed::send_int_value(task_id, -1);

  if (caching_info_ptr != NULL)
    {
      distributable_computation_cache_enabled(
                                              forward_projector_ptr,
                                              back_projector_ptr,
                                              symmetries_ptr,
                                              output_image_ptr,
                                              input_image_ptr,
                                              proj_dat_ptr,
                                              read_from_proj_dat,
                                              subset_num,  num_subsets,
                                              min_segment_num,  max_segment_num,
                                              zero_seg0_end_planes,
                                              log_likelihood_ptr,
                                              binwise_correction,
                                              normalisation_sptr,
                                              start_time_of_frame,
                                              end_time_of_frame,
                                              RPC_process_related_viewgrams,
                                              caching_info_ptr);
      return;
    }


  //test distributed functions (see DistributedTestFunctions.h for details)

#ifndef NDEBUG
  if (distributed::test && distributed::first_iteration) 
    {
      distributed::test_image_estimate_master(input_image_ptr, 1);
      distributed::test_parameter_info_master(proj_dat_ptr->get_proj_data_info_ptr()->parameter_info(), 1, "proj_data_info");
      distributed::test_bool_value_master(true, 1);
      distributed::test_int_value_master(444, 1);
      distributed::test_int_values_master(1);
                
      Viewgram<float> viewgram(proj_dat_ptr->get_proj_data_info_ptr()->create_shared_clone(), 44, 0);
      for ( int tang_pos = viewgram.get_min_tangential_pos_num(); tang_pos  <= viewgram.get_max_tangential_pos_num() ;++tang_pos)  
        for ( int ax_pos = viewgram.get_min_axial_pos_num(); ax_pos <= viewgram.get_max_axial_pos_num() ;++ax_pos)
          viewgram[ax_pos][tang_pos]= rand();
                        
      distributed::test_viewgram_master(viewgram, proj_dat_ptr->get_proj_data_info_ptr()->create_shared_clone());
    }
#endif
        
  //send if log_likelihood_ptr is valid and so needs to be accumulated
  distributed::send_bool_value(!is_null_ptr(log_likelihood_ptr),USE_DOUBLE_ARG_TAG,-1);
  //send the current image estimate
  distributed::send_image_estimate(input_image_ptr, -1);
  //send if output_image_ptr is valid and so needs to be accumulated
  distributed::send_bool_value(!is_null_ptr(output_image_ptr),USE_OUTPUT_IMAGE_ARG_TAG,-1);
        
#endif

  assert(min_segment_num <= max_segment_num);
  assert(subset_num >=0);
  assert(subset_num < num_subsets);
  
  assert(!is_null_ptr(proj_dat_ptr));
  
  if (output_image_ptr != NULL)
    output_image_ptr->fill(0);
  
  if (log_likelihood_ptr != NULL)
    {
      (*log_likelihood_ptr) = 0.0;
    };

  if (zero_seg0_end_planes)
    cerr << "End-planes of segment 0 will be zeroed" << endl;

#ifdef STIR_MPI
  int sent_count=0;                     //counts the work packages sent 
  int working_slaves_count=0; //counts the number of slaves which are currently working
  int next_receiver=1;          //always stores the next slave to be provided with work
#endif
  //double total_seq_rpc_time=0.0; //sums up times used for RPC_process_related_viewgrams

  for (int segment_num = min_segment_num; segment_num <= max_segment_num; segment_num++)
    {
      CPUTimer segment_timer;
      segment_timer.start();
            
      int count=0, count2=0;

      // boolean used to see when to write diagnostic message
      bool first_view_in_segment = true;

      for (int view = proj_dat_ptr->get_min_view_num() + subset_num; 
           view <= proj_dat_ptr->get_max_view_num(); 
           view += num_subsets)
        {
          const ViewSegmentNumbers view_segment_num(view, segment_num);
        
          if (!symmetries_ptr->is_basic(view_segment_num))
            continue;

          if (first_view_in_segment)
            {
              cerr << "Starting to process segment " << segment_num << " (and symmetry related segments)" << endl;
              first_view_in_segment = false;
            }
    
          shared_ptr<RelatedViewgrams<float> > y;
          shared_ptr<RelatedViewgrams<float> > additive_binwise_correction_viewgrams;
          shared_ptr<RelatedViewgrams<float> > mult_viewgrams_sptr;

          get_viewgrams(y, additive_binwise_correction_viewgrams, mult_viewgrams_sptr,
                        proj_dat_ptr, read_from_proj_dat,
                        zero_seg0_end_planes,
                        binwise_correction,
                        normalisation_sptr, start_time_of_frame, end_time_of_frame,
                        symmetries_ptr, view_segment_num);
      
#ifdef STIR_MPI     

          //send viewgrams, the slave will immediatelly start calculation
          send_viewgrams(y, additive_binwise_correction_viewgrams, mult_viewgrams_sptr,
                         next_receiver);
          working_slaves_count++;
          sent_count++;
    
          //give every slave some work before waiting for requests 
          if (sent_count < distributed::num_processors-1) // note: -1 as master doesn't get any viewgrams
            next_receiver++;
          else 
            {
              //wait for available notification
              int int_values[2];
              const MPI_Status status=distributed::receive_int_values(int_values, 2, AVAILABLE_NOTIFICATION_TAG);
              next_receiver=status.MPI_SOURCE;
              working_slaves_count--;
        
              //reduce count values
              count+=int_values[0];
              count2+=int_values[1];
            }
#else // STIR_MPI
          RPC_process_related_viewgrams(forward_projector_ptr,
                                        back_projector_ptr,
                                        output_image_ptr, input_image_ptr, y.get(), count, count2, log_likelihood_ptr, 
                                        additive_binwise_correction_viewgrams.get(),
                                        mult_viewgrams_sptr.get());
                                    
#endif
        }
#ifdef STIR_MPI
      //in case of the last segment, receive remaining available notifications
      if (segment_num == max_segment_num)
        {
          while(working_slaves_count>0)
            {
              int int_values[2];
              const MPI_Status status=distributed::receive_int_values(int_values, 2, AVAILABLE_NOTIFICATION_TAG);
              working_slaves_count--;
            
              //reduce count values
              count+=int_values[0];
              count2+=int_values[1];
            }
        }
#endif

      if (first_view_in_segment != true) // only write message when at least one view was processed
        {
          // TODO this message relies on knowledge of count, count2 which might be inappropriate for 
          // the call-back function
#ifndef STIR_NO_RUNNING_LOG
          cerr<<"\tNumber of (cancelled) singularities: "<<count
              <<"\n\tNumber of (cancelled) negative numerators: "<<count2
              << "\n\tSegment " << segment_num << ": " << segment_timer.value() << "secs" <<endl;
#endif
        }
    }
  
#ifndef STIR_MPI  
  //printf("Total Iteration Wall clock time used for RPC processing: %.6lf seconds\n", total_seq_rpc_time);
#endif

#ifdef STIR_MPI
  //end of iteration processing
  distributed::first_iteration=false;   
        
  //broadcast end of iteration notification
  int int_values[2];  int_values[0]=3; int_values[1]=4; // values are ignored
  distributed::send_int_values(int_values, 2, END_ITERATION_TAG, -1);
        
  //reduce output image
  if (!is_null_ptr(output_image_ptr))
    distributed::reduce_received_output_image(output_image_ptr, 0);
  // and log_likelihood
  if (!is_null_ptr(log_likelihood_ptr))
    {
      double buffer = 0.0;
      MPI_Reduce(&buffer, log_likelihood_ptr, /*size*/1, MPI_DOUBLE, MPI_SUM, /*destination*/ 0, MPI_COMM_WORLD);
    }

  //reduce timed rpc-value
  if(distributed::rpc_time)
    {
      printf("Master: Reducing timer value\n");
      double send = 0;
      double receive;
      MPI_Reduce(&send, &receive, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
      distributed::total_rpc_time+=(receive/(distributed::num_processors-1));
        
      printf("Average time used by slaves for RPC processing: %f secs\n", distributed::total_rpc_time);
      distributed::total_rpc_time_slaves+=receive;
    }
        
#endif
}

END_NAMESPACE_STIR
