/*
 * \brief  Genode/Nova specific VirtualBox SUPLib supplements
 * \author Alexander Boettcher
 * \date   2013-11-18
 */

/*
 * Copyright (C) 2013-2017 Genode Labs GmbH
 *
 * This file is distributed under the terms of the GNU General Public License
 * version 2.
 */

#ifndef _VIRTUALBOX__SPEC__NOVA__VCPU_SVM_H_
#define _VIRTUALBOX__SPEC__NOVA__VCPU_SVM_H_

/* Genode's VirtualBox includes */
#include "vcpu.h"
#include "svm.h"

class Vcpu_handler_svm : public Vcpu_handler
{
	private:

		__attribute__((noreturn)) void _svm_default() { _default_handler(); }

		__attribute__((noreturn)) void _svm_invalid()
		{
			Vmm::error("invalid guest state - dead ?");
			_default_handler();
		}

		__attribute__((noreturn)) void _svm_vintr() { _irq_window(); }

		template <unsigned X>
		__attribute__((noreturn)) void _svm_npt()
		{
			using namespace Nova;
			using namespace Genode;

			Thread *myself = Thread::myself();
			Utcb *utcb = reinterpret_cast<Utcb *>(myself->utcb());

			_exc_memory<X>(myself, utcb, utcb->qual[0] & 1,
			               utcb->qual[1] & ~((1UL << 12) - 1));
		}

		__attribute__((noreturn)) void _svm_startup()
		{
			using namespace Nova;

			/* enable VM exits for CPUID */
			next_utcb.mtd     = Nova::Mtd::CTRL;
			next_utcb.ctrl[0] = SVM_CTRL1_INTERCEPT_INTR
			                  | SVM_CTRL1_INTERCEPT_NMI
			                  | SVM_CTRL1_INTERCEPT_INIT
			                  | SVM_CTRL1_INTERCEPT_RDPMC
			                  | SVM_CTRL1_INTERCEPT_CPUID
			                  | SVM_CTRL1_INTERCEPT_RSM
			                  | SVM_CTRL1_INTERCEPT_HLT
			                  | SVM_CTRL1_INTERCEPT_INOUT_BITMAP
			                  | SVM_CTRL1_INTERCEPT_MSR_SHADOW
			                  | SVM_CTRL1_INTERCEPT_INVLPGA
			                  | SVM_CTRL1_INTERCEPT_SHUTDOWN
			                  | SVM_CTRL1_INTERCEPT_FERR_FREEZE;

			next_utcb.ctrl[1] = SVM_CTRL2_INTERCEPT_VMRUN
			                  | SVM_CTRL2_INTERCEPT_VMMCALL
			                  | SVM_CTRL2_INTERCEPT_VMLOAD
			                  | SVM_CTRL2_INTERCEPT_VMSAVE
			                  | SVM_CTRL2_INTERCEPT_STGI
			                  | SVM_CTRL2_INTERCEPT_CLGI
			                  | SVM_CTRL2_INTERCEPT_SKINIT
			                  | SVM_CTRL2_INTERCEPT_WBINVD
			                  | SVM_CTRL2_INTERCEPT_MONITOR
			                  | SVM_CTRL2_INTERCEPT_MWAIT;

			void *exit_status = _start_routine(_arg);
			pthread_exit(exit_status);

			Nova::reply(nullptr);
		}

		__attribute__((noreturn)) void _svm_recall()
		{
			Vcpu_handler::_recall_handler();
		}

		__attribute__((noreturn)) void _svm_triple()
		{
			Vmm::error("triple fault - dead");
			exit(-1);
		}

	public:

		Vcpu_handler_svm(Genode::Env &env, size_t stack_size, const pthread_attr_t *attr,
		                 void *(*start_routine) (void *), void *arg,
		                 Genode::Cpu_session * cpu_session,
		                 Genode::Affinity::Location location,
		                 unsigned int cpu_id, const char * name,
		                 Genode::Pd_session_capability pd_vcpu)
		:
			Vcpu_handler(env, stack_size, attr, start_routine, arg, cpu_session,
			             location, cpu_id, name, pd_vcpu)
		{
			using namespace Nova;

			Genode::addr_t const exc_base = vcpu().exc_base();

			typedef Vcpu_handler_svm This;

			register_handler<SVM_EXIT_SHUTDOWN, This,
				&This::_svm_triple> (exc_base, Mtd::ALL | Mtd::FPU);
			register_handler<SVM_EXIT_READ_CR0, This,
				&This::_svm_default>      (exc_base, Mtd(Mtd::ALL | Mtd::FPU));
			register_handler<RECALL, This,
				&This::_svm_recall>       (exc_base, Mtd::ALL | Mtd::FPU);
			register_handler<SVM_EXIT_IOIO, This,
				&This::_svm_default>      (exc_base, Mtd(Mtd::ALL | Mtd::FPU));
			register_handler<SVM_EXIT_VINTR, This,
				&This::_svm_vintr>        (exc_base, Mtd(Mtd::ALL | Mtd::FPU));
			register_handler<SVM_EXIT_RDTSC, This,
				&This::_svm_default>      (exc_base, Mtd(Mtd::ALL | Mtd::FPU));
			register_handler<SVM_EXIT_MSR, This,
				&This::_svm_default>      (exc_base, Mtd(Mtd::ALL | Mtd::FPU));
			register_handler<SVM_NPT, This,
				&This::_svm_npt<SVM_NPT>> (exc_base, Mtd(Mtd::ALL | Mtd::FPU));
			register_handler<SVM_EXIT_HLT, This,
				&This::_svm_default>      (exc_base, Mtd(Mtd::ALL | Mtd::FPU));
			register_handler<SVM_EXIT_CPUID, This,
				&This::_svm_default>      (exc_base, Mtd(Mtd::ALL | Mtd::FPU));
			register_handler<VCPU_STARTUP, This,
				&This::_svm_startup>      (exc_base, Mtd(Mtd::ALL | Mtd::FPU));
			register_handler<SVM_EXIT_WBINVD, This,
				&This::_svm_default>      (exc_base, Mtd(Mtd::ALL | Mtd::FPU));

			register_handler<SVM_INVALID, This,
				&This::_svm_invalid>      (exc_base, Mtd(Mtd::ALL | Mtd::FPU));

			start();
		}

		bool hw_save_state(Nova::Utcb * utcb, VM * pVM, PVMCPU pVCpu) {
			return svm_save_state(utcb, pVM, pVCpu);
		}

		bool hw_load_state(Nova::Utcb * utcb, VM * pVM, PVMCPU pVCpu) {
			return svm_load_state(utcb, pVM, pVCpu);
		}

		bool vm_exit_requires_instruction_emulation()
		{
			if (exit_reason == RECALL)
				return false;

			return true;
		}
};

#endif /* _VIRTUALBOX__SPEC__NOVA__VCPU_SVM_H_ */
