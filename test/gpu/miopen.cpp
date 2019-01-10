
#include <migraphx/program.hpp>
#include <migraphx/operators.hpp>
#include <migraphx/generate.hpp>
#include <migraphx/cpu/target.hpp>
#include <migraphx/gpu/target.hpp>
#include <migraphx/gpu/miopen.hpp>
#include <migraphx/gpu/hip.hpp>
#include <migraphx/manage_ptr.hpp>
#include <migraphx/type_name.hpp>
#include <migraphx/verify_args.hpp>
#include <migraphx/instruction.hpp>

#include <miopen/miopen.h>

#include <future>
#include <thread>

#include "test.hpp"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wglobal-constructors"
#endif

// An improved async, that doesn't block
template <class Function>
std::future<typename std::result_of<Function()>::type> detach_async(Function&& f,
                                                                    bool parallel = true)
{
    if(parallel)
    {
        using result_type = typename std::result_of<Function()>::type;
        std::packaged_task<result_type()> task(std::forward<Function>(f));
        auto fut = task.get_future();
        std::thread(std::move(task)).detach();
        return std::move(fut);
    }
    return std::async(std::launch::deferred, std::forward<Function>(f));
}

struct auto_print
{
    static void set_terminate_handler(const std::string& name)
    {
        static std::string pname;
        pname = name;
        std::set_terminate(+[] {
            std::cout << "FAILED: " << pname << std::endl;
            try
            {
                std::rethrow_exception(std::current_exception());
            }
            catch(const std::exception& e)
            {
                std::cout << "    what(): " << e.what() << std::endl;
            }
            std::cout << std::endl;
            for(auto&& handle : auto_print::handlers)
                handle();
        });
    }
    static std::array<std::function<void()>, 2> handlers;
    int index;
    template <class T>
    auto_print(T& x, int i) : index(i)
    {
        handlers[index] = [&x] { std::cout << x << std::endl; };
    }

    ~auto_print()
    {
        handlers[index] = [] {};
    }
};
std::array<std::function<void()>, 2> auto_print::handlers = {};

template <class T>
auto get_hash(const T& x)
{
    return std::hash<T>{}(x);
}

void compile_check(migraphx::program& p, const migraphx::target& t)
{
    auto name = t.name();
    auto s    = p.get_shape();
    std::stringstream ss;
    p.compile(t, migraphx::tracer{ss});
    if(p.get_shape() != s)
    {
        std::cout << ss.str() << std::endl;
        throw std::runtime_error("Compiling program with " + name + " alters its shape");
    }
}

template <class V>
migraphx::argument run_cpu(migraphx::program& p)
{
    V v;
    p = v.create_program();
    auto_print pp{p, 0};
    compile_check(p, migraphx::cpu::target{});
    migraphx::program::parameter_map m;
    for(auto&& x : p.get_parameter_shapes())
    {
        m[x.first] = migraphx::generate_argument(x.second, get_hash(x.first));
    }
    return p.eval(m);
}

template <class V>
migraphx::argument run_gpu(migraphx::program& p)
{
    V v;
    p = v.create_program();
    auto_print pp{p, 1};
    compile_check(p, migraphx::gpu::target{});
    migraphx::program::parameter_map m;
    for(auto&& x : p.get_parameter_shapes())
    {
        m[x.first] =
            migraphx::gpu::to_gpu(migraphx::generate_argument(x.second, get_hash(x.first)));
    }
    EXPECT(bool{m.find("output") != m.end()});
    return migraphx::gpu::from_gpu(p.eval(m));
}

template <class V>
void verify_program()
{
    auto_print::set_terminate_handler(migraphx::get_type_name<V>());
    // std::cout << migraphx::get_type_name<V>() << std::endl;
    migraphx::program cpu_prog;
    migraphx::program gpu_prog;
    auto cpu_arg_f = detach_async([&] { return run_cpu<V>(cpu_prog); });
    auto gpu_arg   = run_gpu<V>(gpu_prog);
    auto cpu_arg   = cpu_arg_f.get();
    bool passed    = verify_args(migraphx::get_type_name<V>(), cpu_arg, gpu_arg);
    if(not passed)
    {
        V v;
        auto p = v.create_program();
        std::cout << p << std::endl;
        std::cout << "cpu:\n" << cpu_prog << std::endl;
        std::cout << "gpu:\n" << gpu_prog << std::endl;
        std::cout << std::endl;
    }
    std::set_terminate(nullptr);
}

struct test_literals
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        auto input = p.add_literal(
            generate_literal(migraphx::shape{migraphx::shape::float_type, {4, 3, 3, 3}}));
        auto weights = p.add_literal(
            generate_literal(migraphx::shape{migraphx::shape::float_type, {4, 3, 3, 3}}));
        auto conv = p.add_instruction(migraphx::op::convolution{}, input, weights);
        p.add_instruction(migraphx::op::relu{}, conv);
        return p;
    }
};

struct test_add
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        migraphx::shape s{migraphx::shape::float_type, {3}};
        auto x = p.add_parameter("x", s);
        auto y = p.add_parameter("y", s);
        p.add_instruction(migraphx::op::add{}, x, y);
        return p;
    }
};

struct test_add_half
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        migraphx::shape s{migraphx::shape::half_type, {3}};
        auto x = p.add_parameter("x", s);
        auto y = p.add_parameter("y", s);
        p.add_instruction(migraphx::op::add{}, x, y);
        return p;
    }
};

struct test_mul
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        migraphx::shape s{migraphx::shape::float_type, {3}};
        auto x = p.add_parameter("x", s);
        auto y = p.add_parameter("y", s);
        p.add_instruction(migraphx::op::mul{}, x, y);
        return p;
    }
};

struct test_exp
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        migraphx::shape s{migraphx::shape::float_type, {6}};
        std::vector<float> data{0.1f, 0.2f, 1.f, 2.f, 0.6f, 10.f};
        auto x = p.add_literal(s, data);
        p.add_instruction(migraphx::op::exp{}, x);
        return p;
    }
};

struct test_log
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        migraphx::shape s{migraphx::shape::float_type, {6}};
        std::vector<float> data{0.1f, 0.2f, 1.f, 2.f, 0.6f, 100.f};
        auto x = p.add_literal(s, data);
        p.add_instruction(migraphx::op::log{}, x);
        return p;
    }
};

struct test_sin
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        migraphx::shape s{migraphx::shape::float_type, {10}};
        auto x = p.add_parameter("x", s);
        p.add_instruction(migraphx::op::sin{}, x);
        return p;
    }
};

struct test_cos
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        migraphx::shape s{migraphx::shape::double_type, {8}};
        auto x = p.add_parameter("x", s);
        p.add_instruction(migraphx::op::cos{}, x);
        return p;
    }
};

struct test_tan
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        migraphx::shape s{migraphx::shape::float_type, {16}};
        auto x = p.add_parameter("x", s);
        p.add_instruction(migraphx::op::tan{}, x);
        return p;
    }
};

struct test_sinh
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        migraphx::shape s{migraphx::shape::double_type, {16}};
        auto x = p.add_parameter("x", s);
        p.add_instruction(migraphx::op::sinh{}, x);
        return p;
    }
};

struct test_cosh
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        migraphx::shape s{migraphx::shape::double_type, {16}};
        auto x = p.add_parameter("x", s);
        p.add_instruction(migraphx::op::cosh{}, x);
        return p;
    }
};

struct test_tanh
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        auto x = p.add_parameter("x", migraphx::shape{migraphx::shape::float_type, {4, 3, 3, 3}});
        p.add_instruction(migraphx::op::tanh{}, x);
        return p;
    }
};

struct test_asin
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        migraphx::shape s{migraphx::shape::double_type, {16}};
        auto x = p.add_parameter("x", s);
        p.add_instruction(migraphx::op::asin{}, x);
        return p;
    }
};

struct test_acos
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        migraphx::shape s{migraphx::shape::double_type, {16}};
        auto x = p.add_parameter("x", s);
        p.add_instruction(migraphx::op::acos{}, x);
        return p;
    }
};

struct test_atan
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        migraphx::shape s{migraphx::shape::double_type, {16}};
        auto x = p.add_parameter("x", s);
        p.add_instruction(migraphx::op::atan{}, x);
        return p;
    }
};

struct test_scale
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        migraphx::shape s{migraphx::shape::float_type, {3}};
        auto x     = p.add_parameter("x", s);
        auto y     = p.add_parameter("y", migraphx::shape::float_type);
        auto scale = p.add_instruction(migraphx::op::scalar{s}, y);
        p.add_instruction(migraphx::op::mul{}, x, scale);
        return p;
    }
};

struct test_slice
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        migraphx::shape s{migraphx::shape::int32_type, {2, 2, 4}};
        auto x      = p.add_parameter("x", s);
        auto y      = p.add_parameter("y", {migraphx::shape::int32_type, {2, 2, 2}});
        auto slice0 = p.add_instruction(migraphx::op::slice{{2}, {0}, {2}}, x);
        p.add_instruction(migraphx::op::add{}, y, slice0);

        return p;
    }
};

struct test_triadd
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        migraphx::shape s{migraphx::shape::float_type, {3}};
        auto x   = p.add_parameter("x", s);
        auto y   = p.add_parameter("y", s);
        auto z   = p.add_parameter("z", s);
        auto sum = p.add_instruction(migraphx::op::add{}, x, y);
        p.add_instruction(migraphx::op::add{}, sum, z);
        return p;
    }
};

struct test_triadd2
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        migraphx::shape s{migraphx::shape::float_type, {2, 3}};
        migraphx::shape b{migraphx::shape::float_type, {3}};
        auto x   = p.add_parameter("x", s);
        auto y   = p.add_parameter("y", s);
        auto z   = p.add_parameter("z", b);
        auto zb  = p.add_instruction(migraphx::op::broadcast{1, s}, z);
        auto sum = p.add_instruction(migraphx::op::add{}, x, y);
        p.add_instruction(migraphx::op::add{}, sum, zb);
        return p;
    }
};

struct test_add_broadcast
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        migraphx::shape s{migraphx::shape::float_type, {3}};
        auto x  = p.add_parameter("x", {migraphx::shape::float_type, {2, 2, 3}});
        auto y  = p.add_parameter("y", {migraphx::shape::float_type, {2, 2}});
        auto by = p.add_instruction(migraphx::op::broadcast{0, x->get_shape()}, y);
        p.add_instruction(migraphx::op::add{}, x, by);
        return p;
    }
};

struct test_add_broadcast2
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        migraphx::shape s{migraphx::shape::float_type, {3}};
        auto x  = p.add_parameter("x", {migraphx::shape::float_type, {2, 3, 4}});
        auto y  = p.add_parameter("y", {migraphx::shape::float_type, {3}});
        auto by = p.add_instruction(migraphx::op::broadcast{1, x->get_shape()}, y);
        p.add_instruction(migraphx::op::add{}, x, by);
        return p;
    }
};

struct test_add_broadcast3
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        migraphx::shape s{migraphx::shape::float_type, {3}};
        auto x  = p.add_parameter("x", {migraphx::shape::float_type, {2, 4, 5}});
        auto y  = p.add_parameter("y", {migraphx::shape::float_type, {4}});
        auto by = p.add_instruction(migraphx::op::broadcast{1, x->get_shape()}, y);
        p.add_instruction(migraphx::op::add{}, x, by);
        return p;
    }
};

struct test_add_broadcast4
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        migraphx::shape s{migraphx::shape::float_type, {3}};
        auto x  = p.add_parameter("x", {migraphx::shape::float_type, {2, 3, 5}});
        auto y  = p.add_parameter("y", {migraphx::shape::float_type, {3}});
        auto by = p.add_instruction(migraphx::op::broadcast{1, x->get_shape()}, y);
        p.add_instruction(migraphx::op::add{}, x, by);
        return p;
    }
};

struct test_add_broadcast5
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        migraphx::shape s{migraphx::shape::float_type, {3}};
        auto x  = p.add_parameter("x", {migraphx::shape::float_type, {2, 4, 8}});
        auto y  = p.add_parameter("y", {migraphx::shape::float_type, {4}});
        auto by = p.add_instruction(migraphx::op::broadcast{1, x->get_shape()}, y);
        p.add_instruction(migraphx::op::add{}, x, by);
        return p;
    }
};

struct test_triadd_broadcast
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        migraphx::shape s{migraphx::shape::float_type, {3}};
        auto x   = p.add_parameter("x", {migraphx::shape::float_type, {2, 2, 3}});
        auto y   = p.add_parameter("y", {migraphx::shape::float_type, {2, 2}});
        auto z   = p.add_parameter("z", {migraphx::shape::float_type, {2, 2, 3}});
        auto by  = p.add_instruction(migraphx::op::broadcast{0, x->get_shape()}, y);
        auto sum = p.add_instruction(migraphx::op::add{}, x, by);
        p.add_instruction(migraphx::op::add{}, sum, z);
        return p;
    }
};

struct test_softmax
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        auto x = p.add_parameter("x", migraphx::shape{migraphx::shape::float_type, {5, 3, 4, 2}});
        p.add_instruction(migraphx::op::softmax{}, x);
        return p;
    }
};

struct test_softmax2
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        auto x =
            p.add_parameter("x", migraphx::shape{migraphx::shape::float_type, {1, 1000, 1, 1}});
        p.add_instruction(migraphx::op::softmax{}, x);
        return p;
    }
};

struct test_conv
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        auto input =
            p.add_parameter("x", migraphx::shape{migraphx::shape::float_type, {4, 3, 3, 3}});
        auto weights =
            p.add_parameter("w", migraphx::shape{migraphx::shape::float_type, {4, 3, 3, 3}});
        p.add_instruction(migraphx::op::convolution{}, input, weights);
        return p;
    }
};

struct test_conv2
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        auto input =
            p.add_parameter("x", migraphx::shape{migraphx::shape::float_type, {1, 512, 28, 28}});
        auto weights =
            p.add_parameter("w", migraphx::shape{migraphx::shape::float_type, {256, 512, 1, 1}});
        p.add_instruction(migraphx::op::convolution{{0, 0}, {1, 1}, {1, 1}}, input, weights);
        return p;
    }
};

struct test_group_conv
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        auto input =
            p.add_parameter("x", migraphx::shape{migraphx::shape::float_type, {1, 4, 16, 16}});
        auto weights =
            p.add_parameter("w", migraphx::shape{migraphx::shape::float_type, {4, 1, 3, 3}});
        migraphx::op::convolution op;
        op.group = 4;
        p.add_instruction(op, input, weights);
        return p;
    }
};

struct test_conv_relu
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        auto input =
            p.add_parameter("x", migraphx::shape{migraphx::shape::float_type, {4, 3, 3, 3}});
        auto weights =
            p.add_parameter("w", migraphx::shape{migraphx::shape::float_type, {4, 3, 3, 3}});
        auto conv = p.add_instruction(migraphx::op::convolution{}, input, weights);
        p.add_instruction(migraphx::op::relu{}, conv);
        return p;
    }
};

struct test_conv_relu_half
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        auto input =
            p.add_parameter("x", migraphx::shape{migraphx::shape::half_type, {4, 3, 3, 3}});
        auto weights =
            p.add_parameter("w", migraphx::shape{migraphx::shape::half_type, {4, 3, 3, 3}});
        auto conv = p.add_instruction(migraphx::op::convolution{}, input, weights);
        p.add_instruction(migraphx::op::relu{}, conv);
        return p;
    }
};

struct test_add_relu
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        auto x   = p.add_parameter("x", migraphx::shape{migraphx::shape::float_type, {4, 3, 3, 3}});
        auto y   = p.add_parameter("y", migraphx::shape{migraphx::shape::float_type, {4, 3, 3, 3}});
        auto add = p.add_instruction(migraphx::op::add{}, x, y);
        p.add_instruction(migraphx::op::relu{}, add);
        return p;
    }
};

struct test_sigmoid
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        auto x = p.add_parameter("x", migraphx::shape{migraphx::shape::float_type, {4, 3, 3, 3}});
        p.add_instruction(migraphx::op::sigmoid{}, x);
        return p;
    }
};

struct test_abs
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        auto x = p.add_parameter("x", migraphx::shape{migraphx::shape::float_type, {4, 3, 3, 3}});
        p.add_instruction(migraphx::op::abs{}, x);
        return p;
    }
};

struct test_leaky_relu
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        auto x = p.add_parameter("x", migraphx::shape{migraphx::shape::float_type, {4, 3, 3, 3}});
        p.add_instruction(migraphx::op::leaky_relu{0.01}, x);
        return p;
    }
};

struct test_elu
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        auto x = p.add_parameter("x", migraphx::shape{migraphx::shape::float_type, {4, 3, 3, 3}});
        p.add_instruction(migraphx::op::leaky_relu{1.0}, x);
        return p;
    }
};

struct test_conv_pooling
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        auto input =
            p.add_parameter("x", migraphx::shape{migraphx::shape::float_type, {4, 3, 32, 32}});
        auto weights =
            p.add_parameter("w", migraphx::shape{migraphx::shape::float_type, {4, 3, 3, 3}});
        auto conv    = p.add_instruction(migraphx::op::convolution{}, input, weights);
        auto pooling = p.add_instruction(migraphx::op::pooling{"max"}, conv);
        p.add_instruction(migraphx::op::relu{}, pooling);
        return p;
    }
};

struct test_global_avg_pooling
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        auto input =
            p.add_parameter("x", migraphx::shape{migraphx::shape::float_type, {1, 3, 16, 16}});
        auto op    = migraphx::op::pooling{"average"};
        auto lens  = input->get_shape().lens();
        op.lengths = {lens[2], lens[3]};
        p.add_instruction(op, input);
        return p;
    }
};

struct test_global_max_pooling
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        auto input =
            p.add_parameter("x", migraphx::shape{migraphx::shape::float_type, {1, 3, 16, 16}});
        auto op    = migraphx::op::pooling{"max"};
        auto lens  = input->get_shape().lens();
        op.lengths = {lens[2], lens[3]};
        p.add_instruction(op, input);
        return p;
    }
};

struct test_gemm
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        auto a = p.add_parameter("a", migraphx::shape{migraphx::shape::float_type, {4, 5}});
        auto b = p.add_parameter("b", migraphx::shape{migraphx::shape::float_type, {5, 3}});
        p.add_instruction(migraphx::op::dot{}, a, b);
        return p;
    }
};

struct test_gemm_half
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        auto a = p.add_parameter("a", migraphx::shape{migraphx::shape::half_type, {4, 5}});
        auto b = p.add_parameter("b", migraphx::shape{migraphx::shape::half_type, {5, 3}});
        p.add_instruction(migraphx::op::dot{}, a, b);
        return p;
    }
};

struct test_gemm_ld
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        auto a =
            p.add_parameter("a", migraphx::shape{migraphx::shape::float_type, {4, 5}, {10, 1}});
        auto b =
            p.add_parameter("b", migraphx::shape{migraphx::shape::float_type, {5, 3}, {20, 1}});
        p.add_instruction(migraphx::op::dot{}, a, b);
        return p;
    }
};

struct test_gemm_transposeb
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        auto a  = p.add_parameter("a", migraphx::shape{migraphx::shape::float_type, {4, 5}});
        auto b  = p.add_parameter("b", migraphx::shape{migraphx::shape::float_type, {3, 5}});
        auto bt = p.add_instruction(migraphx::op::transpose{{1, 0}}, b);
        p.add_instruction(migraphx::op::dot{}, a, bt);
        return p;
    }
};

struct test_gemm_transposea
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        auto a  = p.add_parameter("a", migraphx::shape{migraphx::shape::float_type, {5, 4}});
        auto b  = p.add_parameter("b", migraphx::shape{migraphx::shape::float_type, {5, 3}});
        auto at = p.add_instruction(migraphx::op::transpose{{1, 0}}, a);
        p.add_instruction(migraphx::op::dot{}, at, b);
        return p;
    }
};

struct test_gemm_transposeab
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        auto a  = p.add_parameter("a", migraphx::shape{migraphx::shape::float_type, {5, 4}});
        auto b  = p.add_parameter("b", migraphx::shape{migraphx::shape::float_type, {3, 5}});
        auto at = p.add_instruction(migraphx::op::transpose{{1, 0}}, a);
        auto bt = p.add_instruction(migraphx::op::transpose{{1, 0}}, b);
        p.add_instruction(migraphx::op::dot{}, at, bt);
        return p;
    }
};

struct test_contiguous
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        migraphx::shape s{migraphx::shape::float_type, {4, 4, 4, 3}, {48, 4, 1, 16}};
        auto x = p.add_parameter("x", s);
        p.add_instruction(migraphx::op::contiguous{}, x);
        EXPECT(p.get_shape().standard());
        return p;
    }
};

struct test_transpose
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        migraphx::shape s{migraphx::shape::float_type, {4, 3, 4, 4}};
        auto x                    = p.add_parameter("x", s);
        std::vector<int64_t> perm = {0, 2, 3, 1};
        auto l                    = p.add_instruction(migraphx::op::transpose{perm}, x);
        p.add_instruction(migraphx::op::contiguous{}, l);
        return p;
    }
};

struct test_batchnorm_inference_2
{
    const size_t width    = 14;
    const size_t height   = 14;
    const size_t channels = 256;
    const size_t batches  = 1;

    migraphx::program create_program() const
    {
        migraphx::program p;

        migraphx::shape s{migraphx::shape::float_type, {batches, channels, height, width}};
        migraphx::shape vars{migraphx::shape::float_type, {channels}};
        auto x        = p.add_parameter("x", s);
        auto scale    = p.add_literal(migraphx::abs(migraphx::generate_literal(vars, 1)));
        auto bias     = p.add_literal(migraphx::abs(migraphx::generate_literal(vars, 2)));
        auto mean     = p.add_literal(migraphx::abs(migraphx::generate_literal(vars, 3)));
        auto variance = p.add_literal(migraphx::abs(migraphx::generate_literal(vars, 4)));
        p.add_instruction(migraphx::op::batch_norm_inference{}, x, scale, bias, mean, variance);
        return p;
    }
};

struct test_batchnorm_inference
{
    const size_t width    = 3;
    const size_t height   = 3;
    const size_t channels = 3;
    const size_t batches  = 4;

    migraphx::program create_program() const
    {
        migraphx::program p;

        migraphx::shape s{migraphx::shape::float_type, {batches, channels, height, width}};
        migraphx::shape vars{migraphx::shape::float_type, {channels}};
        auto x        = p.add_parameter("x", s);
        auto scale    = p.add_literal(migraphx::abs(migraphx::generate_literal(vars, 1)));
        auto bias     = p.add_literal(migraphx::abs(migraphx::generate_literal(vars, 2)));
        auto mean     = p.add_literal(migraphx::abs(migraphx::generate_literal(vars, 3)));
        auto variance = p.add_literal(migraphx::abs(migraphx::generate_literal(vars, 4)));
        p.add_instruction(migraphx::op::batch_norm_inference{}, x, scale, bias, mean, variance);
        return p;
    }
};

struct test_conv_bn
{
    migraphx::program create_program() const
    {
        migraphx::program p;

        migraphx::shape xs{migraphx::shape::float_type, {1, 3, 224, 224}};
        migraphx::shape ws{migraphx::shape::float_type, {64, 3, 7, 7}};
        migraphx::shape vars{migraphx::shape::float_type, {64}};
        auto x        = p.add_parameter("x", xs);
        auto w        = p.add_parameter("w", ws);
        auto conv     = p.add_instruction(migraphx::op::convolution{{3, 3}, {2, 2}, {1, 1}}, x, w);
        auto scale    = p.add_literal(migraphx::abs(migraphx::generate_literal(vars, 1)));
        auto bias     = p.add_literal(migraphx::abs(migraphx::generate_literal(vars, 2)));
        auto mean     = p.add_literal(migraphx::abs(migraphx::generate_literal(vars, 3)));
        auto variance = p.add_literal(migraphx::abs(migraphx::generate_literal(vars, 4)));
        p.add_instruction(migraphx::op::batch_norm_inference{}, conv, scale, bias, mean, variance);
        return p;
    }
};

struct test_conv_bn_relu_pooling
{
    migraphx::program create_program() const
    {
        migraphx::program p;

        migraphx::shape xs{migraphx::shape::float_type, {1, 3, 224, 224}};
        migraphx::shape ws{migraphx::shape::float_type, {64, 3, 7, 7}};
        migraphx::shape vars{migraphx::shape::float_type, {64}};
        auto x        = p.add_parameter("x", xs);
        auto w        = p.add_parameter("w", ws);
        auto conv     = p.add_instruction(migraphx::op::convolution{{3, 3}, {2, 2}, {1, 1}}, x, w);
        auto scale    = p.add_literal(migraphx::abs(migraphx::generate_literal(vars, 1)));
        auto bias     = p.add_literal(migraphx::abs(migraphx::generate_literal(vars, 2)));
        auto mean     = p.add_literal(migraphx::abs(migraphx::generate_literal(vars, 3)));
        auto variance = p.add_literal(migraphx::abs(migraphx::generate_literal(vars, 4)));
        auto bn       = p.add_instruction(
            migraphx::op::batch_norm_inference{}, conv, scale, bias, mean, variance);
        auto relu = p.add_instruction(migraphx::op::relu{}, bn);
        p.add_instruction(migraphx::op::pooling{"average", {1, 1}, {2, 2}, {3, 3}}, relu);
        return p;
    }
};

struct test_concat
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        std::size_t axis = 1;
        migraphx::shape s0{migraphx::shape::int32_type, {2, 2}};
        migraphx::shape s1{migraphx::shape::int32_type, {2, 3}};
        migraphx::shape s2{migraphx::shape::int32_type, {2, 1}};
        auto l0 = p.add_parameter("x", s0);
        auto l1 = p.add_parameter("y", s1);
        auto l2 = p.add_parameter("z", s2);
        p.add_instruction(migraphx::op::concat{axis}, l0, l1, l2);
        return p;
    }
};

struct test_concat2
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        std::size_t axis = 0;
        migraphx::shape s0{migraphx::shape::int32_type, {2, 2}};
        migraphx::shape s1{migraphx::shape::int32_type, {3, 2}};
        migraphx::shape s2{migraphx::shape::int32_type, {1, 2}};
        auto l0 = p.add_parameter("x", s0);
        auto l1 = p.add_parameter("y", s1);
        auto l2 = p.add_parameter("z", s2);
        p.add_instruction(migraphx::op::concat{axis}, l0, l1, l2);
        return p;
    }
};

struct test_concat_relu
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        std::size_t axis = 0;
        migraphx::shape s0{migraphx::shape::float_type, {2, 2}};
        migraphx::shape s1{migraphx::shape::float_type, {3, 2}};
        migraphx::shape s2{migraphx::shape::float_type, {1, 2}};
        auto l0 = p.add_parameter("x", s0);
        auto l1 = p.add_parameter("y", s1);
        auto l2 = p.add_parameter("z", s2);
        auto r0 = p.add_instruction(migraphx::op::relu{}, l0);
        auto r1 = p.add_instruction(migraphx::op::relu{}, l1);
        auto r2 = p.add_instruction(migraphx::op::relu{}, l2);
        auto c0 = p.add_instruction(migraphx::op::concat{axis}, r0, r1, r2);
        p.add_instruction(migraphx::op::relu{}, c0);
        return p;
    }
};

void manual_identity()
{
    migraphx::program p;
    std::vector<float> data0 = {0, 1, 2, 3};
    migraphx::shape s0{migraphx::shape::float_type, {2, 2}};
    auto l0 = p.add_literal(migraphx::literal{s0, data0});
    p.add_instruction(migraphx::op::identity{}, l0);
    p.compile(migraphx::gpu::target{});
    migraphx::program::parameter_map m;
    for(auto&& x : p.get_parameter_shapes())
    {
        m[x.first] = migraphx::gpu::to_gpu(migraphx::generate_argument(x.second));
    }
    auto result = migraphx::gpu::from_gpu(p.eval(m));
    std::cout << result << std::endl;
}

void manual_test_concat_relu()
{
    migraphx::program p;
    std::size_t axis         = 0;
    std::vector<float> data0 = {0, 1, 2, 3};
    std::vector<float> data1 = {4, 5, 6, 7, 8, 9};
    std::vector<float> data2 = {10, 11};
    migraphx::shape s0{migraphx::shape::float_type, {2, 2}};
    migraphx::shape s1{migraphx::shape::float_type, {3, 2}};
    migraphx::shape s2{migraphx::shape::float_type, {1, 2}};
    auto l0 = p.add_literal(migraphx::literal{s0, data0});
    auto l1 = p.add_literal(migraphx::literal{s1, data1});
    auto l2 = p.add_literal(migraphx::literal{s2, data2});
    auto r0 = p.add_instruction(migraphx::op::relu{}, l0);
    auto r1 = p.add_instruction(migraphx::op::relu{}, l1);
    auto r2 = p.add_instruction(migraphx::op::relu{}, l2);
    auto c0 = p.add_instruction(migraphx::op::concat{axis}, r0, r1, r2);
    p.add_instruction(migraphx::op::relu{}, c0);

    p.compile(migraphx::gpu::target{});
    migraphx::program::parameter_map m;
    for(auto&& x : p.get_parameter_shapes())
    {
        m[x.first] = migraphx::gpu::to_gpu(migraphx::generate_argument(x.second));
    }
    auto result = migraphx::gpu::from_gpu(p.eval(m));
    std::cout << result << std::endl;
}

struct test_conv_bn_relu_pooling2
{
    static migraphx::instruction_ref
    add_bn(migraphx::program& p, migraphx::instruction_ref x, std::size_t channels)
    {
        migraphx::shape vars{migraphx::shape::float_type, {channels}};
        auto scale = p.add_literal(migraphx::abs(migraphx::generate_literal(vars, 1 + channels)));
        auto bias  = p.add_literal(migraphx::abs(migraphx::generate_literal(vars, 2 + channels)));
        auto mean  = p.add_literal(migraphx::abs(migraphx::generate_literal(vars, 3 + channels)));
        auto variance =
            p.add_literal(migraphx::abs(migraphx::generate_literal(vars, 4 + channels)));
        return p.add_instruction(
            migraphx::op::batch_norm_inference{}, x, scale, bias, mean, variance);
    }
    migraphx::program create_program() const
    {
        migraphx::program p;

        migraphx::shape xs1{migraphx::shape::float_type, {1, 512, 7, 7}};
        migraphx::shape xs2{migraphx::shape::float_type, {1, 1024, 14, 14}};
        migraphx::shape ws1{migraphx::shape::float_type, {2048, 512, 1, 1}};
        migraphx::shape ws2{migraphx::shape::float_type, {2048, 1024, 1, 1}};
        auto x1    = p.add_parameter("x1", xs1);
        auto w1    = p.add_parameter("w1", ws1);
        auto conv1 = p.add_instruction(migraphx::op::convolution{{0, 0}, {1, 1}, {1, 1}}, x1, w1);
        auto bn1   = add_bn(p, conv1, 2048);
        auto x2    = p.add_parameter("x2", xs2);
        auto w2    = p.add_parameter("w2", ws2);
        auto conv2 = p.add_instruction(migraphx::op::convolution{{0, 0}, {2, 2}, {1, 1}}, x2, w2);
        auto bn2   = add_bn(p, conv2, 2048);
        auto add   = p.add_instruction(migraphx::op::add{}, bn1, bn2);
        auto relu  = p.add_instruction(migraphx::op::relu{}, add);
        p.add_instruction(migraphx::op::pooling{"average", {1, 1}, {2, 2}, {3, 3}}, relu);
        return p;
    }
};

int main()
{
    verify_program<test_abs>();
    verify_program<test_concat>();
    verify_program<test_concat2>();
    verify_program<test_concat_relu>();
    verify_program<test_add>();
    verify_program<test_add_half>();
    verify_program<test_mul>();
    verify_program<test_exp>();
    verify_program<test_log>();
    verify_program<test_sin>();
    verify_program<test_cos>();
    verify_program<test_tan>();
    verify_program<test_sinh>();
    verify_program<test_cosh>();
    verify_program<test_tanh>();
    verify_program<test_asin>();
    verify_program<test_acos>();
    verify_program<test_atan>();
    verify_program<test_scale>();
    verify_program<test_triadd>();
    verify_program<test_triadd2>();
    verify_program<test_add_broadcast>();
    verify_program<test_add_broadcast2>();
    verify_program<test_add_broadcast3>();
    verify_program<test_add_broadcast4>();
    verify_program<test_add_broadcast5>();
    verify_program<test_triadd_broadcast>();
    verify_program<test_softmax>();
    verify_program<test_softmax2>();
    verify_program<test_conv>();
    verify_program<test_conv2>();
    verify_program<test_group_conv>();
    verify_program<test_conv_relu>();
    verify_program<test_conv_relu_half>();
    verify_program<test_add_relu>();
    verify_program<test_leaky_relu>();
    verify_program<test_sigmoid>();
    verify_program<test_elu>();
    verify_program<test_conv_pooling>();
    verify_program<test_global_avg_pooling>();
    verify_program<test_global_max_pooling>();
    verify_program<test_gemm>();
    verify_program<test_gemm_half>();
    // verify_program<test_gemm_ld>();
    verify_program<test_gemm_transposeb>();
    verify_program<test_gemm_transposea>();
    verify_program<test_gemm_transposeab>();
    verify_program<test_contiguous>();
    verify_program<test_transpose>();
    verify_program<test_batchnorm_inference>();
    verify_program<test_batchnorm_inference_2>();
    verify_program<test_conv_bn>();
    verify_program<test_conv_bn_relu_pooling>();
    verify_program<test_conv_bn_relu_pooling2>();
    verify_program<test_slice>();
}
