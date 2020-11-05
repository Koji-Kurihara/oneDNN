/*******************************************************************************
* Copyright 2016-2020 Intel Corporation
* Copyright 2020 FUJITSU LIMITED
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#ifndef CPU_AARCH64_JIT_GENERATOR_HPP
#define CPU_AARCH64_JIT_GENERATOR_HPP

#include <limits.h>

#include "common/bit_cast.hpp"
#include "common/type_helpers.hpp"
#include "common/utils.hpp"

#include "cpu/aarch64/cpu_isa_traits.hpp"

#include "cpu/aarch64/jit_utils/jit_utils.hpp"

#define STRUCT_ALIGN(al, ...) __VA_ARGS__ __attribute__((__aligned__(al)))

#define DECLARE_CPU_JIT_AUX_FUNCTIONS(jit_name) \
    const char *name() const override { return STRINGIFY(jit_name); } \
    const char *source_file() const override { return __FILE__; }

namespace dnnl {
namespace impl {
namespace cpu {
namespace aarch64 {

// TODO: move this to jit_generator class?
namespace {

typedef enum {
    MAX_CODE_SIZE = 256 * 1024,
} max_code_size_t;

// TODO: move this somewhere else? Although this is only used by jit kernels
// (Roma)
static inline int float2int(float x) {
    return utils::bit_cast<int>(x);
}

// Callee-saved registers
constexpr Xbyak_aarch64::Operand::Code abi_save_gpr_regs[]
        = {Xbyak_aarch64::Operand::X19, Xbyak_aarch64::Operand::X20,
                Xbyak_aarch64::Operand::X21, Xbyak_aarch64::Operand::X22,
                Xbyak_aarch64::Operand::X23, Xbyak_aarch64::Operand::X24,
                Xbyak_aarch64::Operand::X25, Xbyak_aarch64::Operand::X26,
                Xbyak_aarch64::Operand::X27, Xbyak_aarch64::Operand::X28};

// See "Procedure Call Standsard for the ARM 64-bit Architecture (AArch64)"
static const Xbyak_aarch64::XReg abi_param1(Xbyak_aarch64::Operand::X0),
        abi_param2(Xbyak_aarch64::Operand::X1),
        abi_param3(Xbyak_aarch64::Operand::X2),
        abi_param4(Xbyak_aarch64::Operand::X3),
        abi_param5(Xbyak_aarch64::Operand::X4),
        abi_param6(Xbyak_aarch64::Operand::X5),
        abi_param7(Xbyak_aarch64::Operand::X6),
        abi_param8(Xbyak_aarch64::Operand::X7),
        abi_not_param1(Xbyak_aarch64::Operand::X15);
} // namespace

class jit_generator : public Xbyak_aarch64::CodeGenerator, public c_compatible {
public:
    using c_compatible::operator new;
    using c_compatible::operator new[];
    using c_compatible::operator delete;
    using c_compatible::operator delete[];

private:
    const size_t xreg_len = 8;
    const size_t vreg_len_preserve = 8; // Only bottom 8byte must be preserved.
    const size_t vreg_to_preserve = 8; // VREG8 - VREG15

    const size_t num_abi_save_gpr_regs
            = sizeof(abi_save_gpr_regs) / sizeof(abi_save_gpr_regs[0]);

    const size_t preserved_stack_size = xreg_len * (2 + num_abi_save_gpr_regs)
            + vreg_len_preserve * vreg_to_preserve;

    const size_t size_of_abi_save_regs = num_abi_save_gpr_regs * x0.getBit() / 8
            + vreg_to_preserve * vreg_len_preserve;

public:
    enum {
        _cmp_eq_oq = 0u,
        _cmp_lt_os = 1u,
        _cmp_le_os = 2u,
        _cmp_neq_uq = 4u,
        _cmp_nlt_us = 5u,
        _cmp_nle_us = 6u,

        _op_floor = 1u,
        _op_mxcsr = 4u,
    };
  
    Xbyak_aarch64::WReg W_TMP_0 = w23;
    Xbyak_aarch64::WReg W_TMP_1 = w24;
    Xbyak_aarch64::WReg W_TMP_2 = w25;
    Xbyak_aarch64::WReg W_TMP_3 = w26;
    Xbyak_aarch64::WReg W_TMP_4 = w27;
    Xbyak_aarch64::XReg X_TMP_0 = x23;
    Xbyak_aarch64::XReg X_TMP_1 = x24;
    Xbyak_aarch64::XReg X_TMP_2 = x25;
    Xbyak_aarch64::XReg X_TMP_3 = x26;
    Xbyak_aarch64::XReg X_TMP_4 = x27;
    Xbyak_aarch64::XReg X_TMP_ADDR = x28;
    const Xbyak_aarch64::XReg X_DEFAULT_ADDR = x28;
    Xbyak_aarch64::PReg P_TMP = p0;
    Xbyak_aarch64::PReg P_TMP_0 = p11;
    Xbyak_aarch64::PReg P_TMP_1 = p12;
    Xbyak_aarch64::PReg P_ALL_ZERO = p10;
    Xbyak_aarch64::PReg P_MSB_256 = p13;
    Xbyak_aarch64::PReg P_MSB_384 = p14;
    Xbyak_aarch64::PReg P_ALL_ONE = p15;

    Xbyak_aarch64::XReg param1 = abi_param1;

    inline size_t get_size_of_abi_save_regs() { return size_of_abi_save_regs; }

    void preamble() {
        stp(x29, x30, pre_ptr(sp, -16));
        /* x29 is a frame pointer. */
        mov(x29, sp);
        sub(sp, sp, static_cast<int64_t>(preserved_stack_size) - 16);

        /* x9 can be used as a temporal register. */
        mov(x9, sp);

        if (vreg_to_preserve) {
            st4((v8.d - v11.d)[0], post_ptr(x9, vreg_len_preserve * 4));
            st4((v12.d - v15.d)[0], post_ptr(x9, vreg_len_preserve * 4));
        }
        for (size_t i = 0; i < num_abi_save_gpr_regs; i += 2) {
            stp(Xbyak_aarch64::XReg(abi_save_gpr_regs[i]),
                    Xbyak_aarch64::XReg(abi_save_gpr_regs[i + 1]),
                    post_ptr(x9, xreg_len * 2));
        }

        ptrue(P_ALL_ONE.b);
        ptrue(P_MSB_384.b, Xbyak_aarch64::VL16);
        ptrue(P_MSB_256.b, Xbyak_aarch64::VL32);
        not_(P_MSB_384.b, P_ALL_ONE / Xbyak_aarch64::T_z, P_MSB_384.b);
        not_(P_MSB_256.b, P_ALL_ONE / Xbyak_aarch64::T_z, P_MSB_256.b);
        pfalse(P_ALL_ZERO.b);
    }

    void postamble() {
        mov(x9, sp);
        eor(P_ALL_ONE.b, P_ALL_ONE / Xbyak_aarch64::T_z, P_ALL_ONE.b,
                P_ALL_ONE.b);
        eor(P_MSB_384.b, P_MSB_384 / Xbyak_aarch64::T_z, P_MSB_384.b,
                P_MSB_384.b);
        eor(P_MSB_256.b, P_MSB_256 / Xbyak_aarch64::T_z, P_MSB_256.b,
                P_MSB_256.b);

        if (vreg_to_preserve) {
            ld4((v8.d - v11.d)[0], post_ptr(x9, vreg_len_preserve * 4));
            ld4((v12.d - v15.d)[0], post_ptr(x9, vreg_len_preserve * 4));
        }

        for (size_t i = 0; i < num_abi_save_gpr_regs; i += 2) {
            ldp(Xbyak_aarch64::XReg(abi_save_gpr_regs[i]),
                    Xbyak_aarch64::XReg(abi_save_gpr_regs[i + 1]),
                    post_ptr(x9, xreg_len * 2));
        }

        add(sp, sp, static_cast<int64_t>(preserved_stack_size) - 16);
        ldp(x29, x30, post_ptr(sp, 16));
        ret();
    }

    // Disallow char-based labels completely
    void L(const char *label) = delete;
    void L(Xbyak_aarch64::Label &label) {
        Xbyak_aarch64::CodeGenerator::L(label);
    }

    void L_aligned(Xbyak_aarch64::Label &label, int alignment = 16) {
        align(alignment);
        L(label);
    }

    /*
      Saturation facility functions. enable to prepare the register
      holding the saturation upperbound and apply the saturation on
      the floating point register
     */
    template <typename Vmm>
    void init_saturate_f32(Vmm vmm_lbound, Vmm vmm_ubound,
            Xbyak_aarch64::XReg reg_tmp, data_type_t idt, data_type_t odt) {
        using namespace data_type;
        if (!((idt == f32) && utils::one_of(odt, u8, s8, s32))) return;

        assert(IMPLICATION(
                idt == u8, vmm_lbound.getIdx() != vmm_ubound.getIdx()));
        // No need to saturate on lower bound for signed integer types, as
        // the conversion to int would return INT_MIN, and then proper
        // saturation will happen in store_data
        if (odt == u8) {
            if (mayiuse(sve_512))
                dup(Xbyak_aarch64::ZRegS(vmm_lbound.getIdx()), 0);
            else if (mayiuse(asimd))
                movi(Xbyak_aarch64::VReg4S(vmm_lbound.getIdx()), 0);
            else
                assert(!"unreachable");
        }

        Xbyak_aarch64::ZRegS z_tmp(vmm_ubound.getIdx());
        Xbyak_aarch64::WReg w_tmp(reg_tmp.getIdx());
        float saturation_ubound = types::max_value<float>(odt);
        mov_imm(w_tmp, float2int(saturation_ubound));
        dup(z_tmp, w_tmp);
    }

    template <typename Vmm>
    void saturate_f32(const Vmm &vmm, const Vmm &vmm_lbound,
            const Vmm &vmm_ubound, data_type_t odt,
            const Xbyak_aarch64::PReg &p_true) {
        // This function is used to saturate to odt in f32 before converting
        // to s32 in order to avoid bad saturation due to cvtps2dq
        // behavior (it returns INT_MIN if the f32 is out of the
        // s32 range)
        using namespace data_type;
        if (!utils::one_of(odt, u8, s8, s32)) return;

        Xbyak_aarch64::VReg4S v_tmp(vmm.getIdx());
        Xbyak_aarch64::VReg4S v_lbound(vmm_lbound.getIdx());
        Xbyak_aarch64::VReg4S v_ubound(vmm_ubound.getIdx());
        Xbyak_aarch64::ZRegS z_tmp(vmm.getIdx());
        Xbyak_aarch64::ZRegS z_lbound(vmm_lbound.getIdx());
        Xbyak_aarch64::ZRegS z_ubound(vmm_ubound.getIdx());

        // no need to apply lower saturation bound when odt is
        // signed, as cvtps2dq will return MIN_INT if the value
        // does not fit
        if (odt == u8) {
            if (mayiuse(sve_512))
                fmax(z_tmp, p_true / Xbyak_aarch64::T_m, z_lbound);
            else if (mayiuse(asimd))
                fmax(v_tmp, v_tmp, v_lbound);
            else
                assert(!"unreachable");
        }
        if (mayiuse(sve_512))
            fmin(z_tmp, p_true / Xbyak_aarch64::T_m, z_ubound);
        else if (mayiuse(asimd))
            fmin(v_tmp, v_tmp, v_ubound);
        else
            assert(!"unreachable");
    }

    DNNL_DISALLOW_COPY_AND_ASSIGN(jit_generator);

public:
    jit_generator(void *code_ptr = nullptr, size_t code_size = MAX_CODE_SIZE,
            bool use_autogrow = true)
        : Xbyak_aarch64::CodeGenerator(code_size, code_ptr) {}
    virtual ~jit_generator() {}

    virtual const char *name() const = 0;
    virtual const char *source_file() const = 0;

    void register_jit_code(const uint8_t *code, size_t code_size) const {
        jit_utils::register_jit_code(code, code_size, name(), source_file());
    }

    const uint8_t *jit_ker() const { return jit_ker_; }

    template <typename... kernel_args_t>
    void operator()(kernel_args_t... args) const {
        using jit_kernel_func_t = void (*)(const kernel_args_t... args);
        auto *fptr = (jit_kernel_func_t)jit_ker_;
        (*fptr)(std::forward<kernel_args_t>(args)...);
    }

    virtual status_t create_kernel() {
        generate();
        jit_ker_ = getCode();
        return (jit_ker_) ? status::success : status::runtime_error;
    }

private:
    const uint8_t *getCode() {
        this->ready();
        if (!is_initialized()) return nullptr;
        const uint8_t *code
                = reinterpret_cast<const uint8_t *>(CodeGenerator::getCode());
        register_jit_code(code, getSize());
        return code;
    }

    static inline bool is_initialized() {
        /* At the moment, Xbyak_aarch64 does not have GetError()\
         so that return dummy result. */
        return true;
    }

protected:
    virtual void generate() = 0;
    const uint8_t *jit_ker_ = nullptr;
};

} // namespace aarch64
} // namespace cpu
} // namespace impl
} // namespace dnnl

#endif
