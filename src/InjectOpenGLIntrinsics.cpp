#include "InjectOpenGLIntrinsics.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "CodeGen_GPU_Dev.h"
#include "Substitute.h"
#include "FuseGPUThreadLoops.h"

namespace Halide {
namespace Internal {

using std::string;
using std::vector;

/** Normalizes coordinate loads/stores and produces glsl_texture_load/stores. */
class InjectOpenGLIntrinsics : public IRMutator {
public:
    InjectOpenGLIntrinsics()
        : inside_kernel_loop(false) {
    }
    Scope<int> scope;
    bool inside_kernel_loop;
private:
    using IRMutator::visit;

    void visit(const Call *call) {
        if (call->call_type != Call::Intrinsic) {
            IRMutator::visit(call);
            return;
        }
        if (call->name == Call::coordinates_load) {
            vector<Expr> call_args = call->args;
            //
            // Create
            //  glsl_texture_load("name",
            //                    name.buffer,
            //                    x/x_extent,
            //                    y/y_extent,
            //                    c)
            // from
            //  coordinates_load("name",
            //                   name.buffer,
            //                   x, x_extent,
            //                   y, y_extent,
            //                   c, c_extent
            //                   )
            //
            vector<Expr> args(5);
            args[0] = call_args[0];    // "name"
            args[1] = call_args[1];    // name.buffer

            // Normalize first two coordinates.
            for (size_t i = 0; i < 2; i++) {
                int to_index = 2 + i;
                int from_index = 2 + i * 2;
                args[to_index] =
                  (Cast::make(Float(32), mutate(call_args[from_index])) + 0.5f) /
                  mutate(call_args[from_index + 1]);
            }

            Expr c_coordinate = mutate(call_args[2 + 2 * 2]);
            args[4] = c_coordinate;

            Type load_type = call->type;
            load_type.width = 4;

            Expr load_call = Call::make(load_type, Call::glsl_texture_load,
                                        vector<Expr>(&args[0], &args[4]),
                                        Call::Intrinsic, Function(), 0,
                                        call->image, call->param);

            // Add a shuffle_vector intrinsic to swizzle a single channel
            // scalar out of the vec4 loaded by glsl_texture_load. This may
            // be widened to the size of the Halide function color dimension
            // during vectorization.
            expr = Call::make(call->type, Call::shuffle_vector,
                              vec(load_call, c_coordinate), Call::Intrinsic);
        } else if (call->name == Call::coordinates_store) {
            user_assert(call->args.size() == 6)
                << "GLSL stores require three coordinates.\n";

            // Create
            //    gl_texture_store(name, name.buffer, x, y, c, value)
            // out of
            //    coordinate_store(name, name.buffer, x, y, c, value)
            vector<Expr> args(call->args);
            args[5] = mutate(call->args[5]); // mutate value
            expr = Call::make(call->type, Call::glsl_texture_store,
                              args, Call::Intrinsic);
        } else {
            IRMutator::visit(call);
        }
    }
};

Stmt inject_opengl_intrinsics(Stmt s) {
    InjectOpenGLIntrinsics gl;
    return gl.mutate(s);
}

}
}
