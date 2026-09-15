"""Exercise the production SPIR-V constant-buffer declaration builder."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
STUB = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <climits>
#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
using u32=std::uint32_t; using Id=u32;
// The production file takes static_vector from boost; capacity is irrelevant here.
namespace boost::container { template<class T,std::size_t N> using static_vector=std::vector<T>; }
namespace spv {
enum class Decoration { ArrayStride, Block, Offset, Binding, DescriptorSet };
enum class StorageClass { Uniform };
}
namespace Common { template<class T> T DivCeil(T a,T b) { return (a+b-1)/b; } }
namespace fmt { inline std::string format(std::string_view,u32) { return {}; } }
struct UniformDefinitions { Id field{}; };
struct ConstantBufferDescriptor { u32 index,count; };
struct Info {
 static constexpr size_t MAX_CBUFS{18};
 std::vector<ConstantBufferDescriptor> constant_buffer_descriptors;
 std::array<u32,18> constant_buffer_used_sizes{};
 bool uses_global_memory{};
};
struct EmitContext {
 struct { u32 supported_spirv=0x10400; } profile;
 int stage=0;
 UniformDefinitions uniform_types;
 std::array<UniformDefinitions,18> cbufs{};
 std::vector<Id> interfaces;
 std::vector<u32> lengths;
 Id next=10;
 Id Const(u32 value) { return value; }
 Id TypeArray(Id,u32 count) { lengths.push_back(count); return next++; }
 Id TypeStruct(Id) { return next++; }
 Id TypePointer(spv::StorageClass,Id) { return next++; }
 Id AddGlobalVariable(Id,spv::StorageClass) { return next++; }
 template<class... T> void Decorate(T...) {}
 template<class... T> void MemberName(T...) {}
 template<class... T> void MemberDecorate(T...) {}
 template<class... T> void Name(T...) {}
};
template<class... T> void Name(EmitContext&,T...) {}
'''
CHECKS = r'''
int main() {
 for(u32 width : {1u,2u,4u,8u,16u}) {
  Info info;
  info.constant_buffer_descriptors={{3,1},{5,1},{7,1}};
  info.constant_buffer_used_sizes[3]=256;
  // Dynamic addressing already requires the full guest constant-buffer range.
  info.constant_buffer_used_sizes[5]=65536;
  EmitContext ctx;
  DefineConstBuffers(ctx,info,&UniformDefinitions::field,0,1,'u',width);
  // Three distinct lengths, so three array types, each declared once.
  assert((ctx.lengths==std::vector<u32>{256/width,65536/width,1}));
  assert(ctx.interfaces.size()==3);
  assert(ctx.cbufs[3].field && ctx.cbufs[5].field && ctx.cbufs[7].field);
  // Every buffer still gets its own variable.
  assert(ctx.cbufs[3].field!=ctx.cbufs[5].field);
  assert(ctx.cbufs[5].field!=ctx.cbufs[7].field);

  // Equal lengths must collapse to ONE array type. SPIR-V deduplicates types,
  // so emitting the declaration per descriptor would decorate the same id with
  // ArrayStride/Block/Offset repeatedly, which spirv-val rejects with
  // "decorated with ArrayStride multiple times".
  info.uses_global_memory=true;
  EmitContext global;
  DefineConstBuffers(global,info,&UniformDefinitions::field,0,1,'u',width);
  assert((global.lengths==std::vector<u32>{65536/width}));
  assert(global.interfaces.size()==3);

  // Same again without the global-memory fallback: two buffers of equal
  // recorded size share one declaration, a third distinct size adds one.
  Info shared;
  shared.constant_buffer_descriptors={{0,1},{1,1},{2,1}};
  shared.constant_buffer_used_sizes[0]=512;
  shared.constant_buffer_used_sizes[1]=512;
  shared.constant_buffer_used_sizes[2]=1024;
  EmitContext dedup;
  DefineConstBuffers(dedup,shared,&UniformDefinitions::field,0,1,'u',width);
  assert((dedup.lengths==std::vector<u32>{512/width,1024/width}));
  assert(dedup.interfaces.size()==3);
 }
}
'''

class ShaderConstantBuffers(unittest.TestCase):
    def test_declared_ranges_match_shader_usage(self):
        source=(ROOT/'src/shader_recompiler/backend/spirv/spirv_emit_context.cpp').read_text()
        start=source.index('void DefineConstBuffers(')
        end=source.index('\nvoid DefineSsbos(',start)
        with tempfile.TemporaryDirectory(prefix='ihorizon-cbuf-') as tmp:
            cpp=Path(tmp)/'test.cpp'
            binary=Path(tmp)/'test'
            cpp.write_text(STUB+source[start:end]+CHECKS)
            subprocess.run([os.environ.get('CXX','c++'),'-std=c++20',str(cpp),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

if __name__=='__main__': unittest.main(verbosity=2)
