// MIT License
//
// Copyright (c) 2026 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"

#include "dxbc.h"
#include "log.h"
#include "shaders.h"

namespace {

    const char* CustomSemanticTypeName(NV_CUSTOM_SEMANTIC_TYPE type) {
        switch (type) {
        case NV_X_RIGHT_SEMANTIC:
            return "X_RIGHT";
        case NV_VIEWPORT_MASK_SEMANTIC:
            return "VIEWPORT_MASK";
        case NV_XYZW_RIGHT_SEMANTIC:
            return "XYZW_RIGHT";
        case NV_VIEWPORT_MASK_2_SEMANTIC:
            return "VIEWPORT_MASK_2";
        case NV_POSITION_SEMANTIC:
            return "POSITION";
        case NV_CLIP_DISTANCE_0_SEMANTIC:
            return "CLIP_DISTANCE_0";
        case NV_CLIP_DISTANCE_1_SEMANTIC:
            return "CLIP_DISTANCE_1";
        case NV_CULL_DISTANCE_0_SEMANTIC:
            return "CULL_DISTANCE_0";
        case NV_CULL_DISTANCE_1_SEMANTIC:
            return "CULL_DISTANCE_1";
        case NV_GENERIC_ATTRIBUTE_SEMANTIC:
            return "GENERIC_ATTRIBUTE";
        case NV_PACKED_EYE_INDEX_SEMANTIC:
            return "PACKED_EYE_INDEX";
        default:
            return "?";
        }
    }

    std::string MaskToString(uint8_t mask) {
        std::string result;
        for (int i = 0; i < 4; i++) {
            if (mask & (1 << i)) {
                result += "xyzw"[i];
            }
        }
        return result;
    }

    // A short identifier for the shader, from its DXBC checksum.
    std::string ShaderHash(const uint8_t* bytecode) {
        char hash[33];
        for (int i = 0; i < 16; i++) {
            sprintf_s(hash + i * 2, 3, "%02x", bytecode[4 + i]);
        }
        return hash;
    }

    void LogSignature(const dxbc::Signature& signature) {
        LOG_DETAIL("  %s:\n", dxbc::FourCCToString(signature.fourCC).c_str());
        for (const auto& element : signature.elements) {
            LOG_DETAIL("    %s%u: reg=%u mask=%s rw=0x%x sv=%u type=%u stream=%u\n",
                         element.semanticName.c_str(),
                         element.semanticIndex,
                         element.registerIndex,
                         MaskToString(element.mask).c_str(),
                         element.readWriteMask,
                         element.systemValue,
                         element.componentType,
                         element.stream);
        }
    }

    // Writes the disassembly of a shader to %LOCALAPPDATA%\AMD-MVR-Adapter\shaders, in Debug builds only.
    void DumpDisassembly(const std::string& name, const void* bytecode, size_t length) {
#if !defined(_DEBUG) && !defined(MVR_DIAG)
        (void)name;
        (void)bytecode;
        (void)length;
#else
#ifdef MVR_DIAG
        // Enough to study how the views are computed, without writing hundreds of files while the game loads.
        static std::atomic<int> s_dumps{0};
        if (s_dumps.fetch_add(1) >= 80) {
            return;
        }
#endif
        const std::wstring dataDirectory = logging::GetDataDirectory();
        if (dataDirectory.empty()) {
            return;
        }
        const std::wstring directory = dataDirectory + L"\\shaders";
        CreateDirectoryW(directory.c_str(), nullptr);
        const std::wstring path = directory + L"\\" + std::wstring(name.begin(), name.end()) + L".asm";

        Microsoft::WRL::ComPtr<ID3DBlob> disassembly;
        if (FAILED(D3DDisassemble(bytecode, length, 0, nullptr, &disassembly))) {
            logging::Log("  Failed to disassemble the shader\n");
            return;
        }
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"wb") == 0 && file) {
            fwrite(disassembly->GetBufferPointer(), 1, disassembly->GetBufferSize(), file);
            fclose(file);
        }
#endif
    }

    // The resource and sampler slots that AmdDxExtShaderIntrinsics_GetViewportIndex() samples from, and the
    // instruction it encodes in the LOD: MakeAmdShaderIntrinsicsInstruction(AmdDxExtShaderIntrinsicsOpcode_ViewportIndex)
    // (see ags_shader_intrinsics_dx11.hlsl).
    constexpr uint32_t kIntrinsicResourceSlot = 127;
    constexpr uint32_t kIntrinsicSamplerSlot = 15;

    // The instructions implementing AmdDxExtShaderIntrinsics_GetViewportIndex(), taken from a shader compiled with fxc,
    // so that they match exactly what the AMD driver recognizes.
    struct ViewportIndexIntrinsic {
        bool valid = false;
        dxbc::Instruction dclSampler;
        dxbc::Instruction dclResource;
        dxbc::Instruction sample; // Its destination is an output register, to be retargeted.
    };

    ViewportIndexIntrinsic CompileViewportIndexIntrinsic() {
        static constexpr char kSource[] = R"(
SamplerState AmdDxExtShaderIntrinsicsSamplerState : register(s15);
Texture3D<float4> AmdDxExtShaderIntrinsicsResource : register(t127);
float4 main() : SV_Position {
    return AmdDxExtShaderIntrinsicsResource.SampleLevel(AmdDxExtShaderIntrinsicsSamplerState, float3(0, 0, 0), asfloat(0x50000010u));
}
)";
        ViewportIndexIntrinsic intrinsic;
        Microsoft::WRL::ComPtr<ID3DBlob> blob;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        if (FAILED(D3DCompile(kSource,
                              sizeof(kSource) - 1,
                              "GetViewportIndex",
                              nullptr,
                              nullptr,
                              "main",
                              "vs_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3,
                              0,
                              &blob,
                              &errors))) {
            logging::Log("Failed to compile the viewport index intrinsic: %s\n",
                         errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
            return intrinsic;
        }

        dxbc::Container container;
        dxbc::Program program;
        const dxbc::Chunk* chunk = nullptr;
        if (container.Parse(blob->GetBufferPointer(), blob->GetBufferSize())) {
            chunk = container.FindChunk(dxbc::MakeFourCC('S', 'H', 'E', 'X'));
        }
        if (!chunk || !program.Parse(*chunk)) {
            logging::Log("Failed to parse the viewport index intrinsic\n");
            return intrinsic;
        }
        bool hasSampler = false, hasResource = false, hasSample = false;
        for (const auto& instruction : program.instructions) {
            switch (instruction.Opcode()) {
            case dxbc::opcode::DclSampler:
                intrinsic.dclSampler = instruction;
                hasSampler = true;
                break;
            case dxbc::opcode::DclResource:
                intrinsic.dclResource = instruction;
                hasResource = true;
                break;
            case dxbc::opcode::SampleL:
                intrinsic.sample = instruction;
                hasSample = true;
                break;
            }
        }
        intrinsic.valid = hasSampler && hasResource && hasSample;
        if (!intrinsic.valid) {
            logging::Log("Unexpected code for the viewport index intrinsic\n");
        }
        return intrinsic;
    }

    const ViewportIndexIntrinsic& GetViewportIndexIntrinsic() {
        static const ViewportIndexIntrinsic intrinsic = CompileViewportIndexIntrinsic();
        return intrinsic;
    }

    // An output holding the position for one view other than view 0, declared through an NVIDIA custom semantic.
    struct ViewOutput {
        uint32_t view = 0;
        uint32_t registerIndex = 0;
        uint8_t mask = 0;
        bool xOnly = false; // NV_X_RIGHT: only the X coordinate differs from view 0.
        uint32_t temp = 0;  // The temp register replacing the output while the shader runs.
    };

    // Find the outputs carrying the positions of the other views.
    std::vector<ViewOutput> FindViewOutputs(const dxbc::Signature& outputSignature,
                                            NvU32 numCustomSemantics,
                                            const NV_CUSTOM_SEMANTIC* customSemantics) {
        std::vector<ViewOutput> views;
        for (const auto& element : outputSignature.elements) {
            for (NvU32 i = 0; i < numCustomSemantics; i++) {
                const NV_CUSTOM_SEMANTIC& semantic = customSemantics[i];
                const std::string name = semantic.NVCustomSemanticNameString;
                ViewOutput view;
                view.registerIndex = element.registerIndex;
                view.mask = element.mask;

                switch (semantic.NVCustomSemanticType) {
                case NV_X_RIGHT_SEMANTIC:
                case NV_XYZW_RIGHT_SEMANTIC:
                    if (_stricmp(element.semanticName.c_str(), name.c_str()) != 0) {
                        continue;
                    }
                    view.view = 1;
                    view.xOnly = semantic.NVCustomSemanticType == NV_X_RIGHT_SEMANTIC;
                    break;

                case NV_POSITION_SEMANTIC:
                    // Either the semantic index is the view ("NV_POSITION1"), or the view follows the name
                    // ("NV_POSITION_VIEW_1_SEMANTIC").
                    if (_stricmp(element.semanticName.c_str(), name.c_str()) == 0) {
                        view.view = element.semanticIndex;
                    } else {
                        const std::string prefix = name + "_VIEW_";
                        if (element.semanticName.size() <= prefix.size() ||
                            _strnicmp(element.semanticName.c_str(), prefix.c_str(), prefix.size()) != 0 ||
                            !isdigit(static_cast<unsigned char>(element.semanticName[prefix.size()]))) {
                            continue;
                        }
                        view.view = element.semanticName[prefix.size()] - '0';
                    }
                    if (view.view == 0) {
                        continue;
                    }
                    break;

                default:
                    continue;
                }
                views.push_back(view);
            }
        }
        return views;
    }

    // Rewrite a vertex shader written for NVIDIA single pass stereo or multi-view rendering, to run once per view with
    // AGS view broadcasting.
    //
    // NVIDIA runs the shader once and takes the position of each view from a different output (SV_Position for view 0,
    // and the custom semantics for the others). AGS runs the shader once per view, and the shader must output the
    // position of the view it runs for. So we "un-loop" the shader: it still computes every position, then picks the
    // one for the current view, given by AmdDxExtShaderIntrinsics_GetViewportIndex(). That view index is also
    // written to SV_ViewportArrayIndex, which the pixel shaders of such applications read (normally written by the
    // NVIDIA fast geometry shader, that we drop).
    //
    // Geometry shaders written for SPS/MVR output the positions of the views the same way, for each vertex they emit:
    // the same rewrite applies, with the selection done before each emit instead of before returning.
    //
    // Returns false if the shader does not need patching, or cannot be patched. In both cases, summary describes the
    // outcome, for the log.
    bool PatchShader(dxbc::Container& container,
                     NvU32 numCustomSemantics,
                     const NV_CUSTOM_SEMANTIC* customSemantics,
                     std::string& summary) {
        const auto notPatched = [&summary](const std::string& reason) {
            summary = "not rewritten (" + reason + ")";
            return false;
        };
        using namespace dxbc;

        Chunk* outputChunk = nullptr;
        Chunk* programChunk = nullptr;
        for (auto& chunk : container.chunks) {
            const uint32_t type = chunk.fourCC;
            if (type == MakeFourCC('O', 'S', 'G', 'N') || type == MakeFourCC('O', 'S', 'G', '5') ||
                type == MakeFourCC('O', 'S', 'G', '1')) {
                outputChunk = &chunk;
            } else if (Program::IsProgramChunk(type)) {
                programChunk = &chunk;
            }
        }
        Signature outputSignature;
        Program program;
        if (!outputChunk || !programChunk || !outputSignature.Parse(*outputChunk) || !program.Parse(*programChunk)) {
            return notPatched("no output signature or program");
        }

        std::vector<ViewOutput> views = FindViewOutputs(outputSignature, numCustomSemantics, customSemantics);

        // The custom semantics given by the application do not always match the outputs of the shader: iRacing
        // creates some multi-view geometry shaders (outputting NV_POSITION_VIEW_n_SEMANTIC) with the semantics of
        // single pass stereo (NV_X_RIGHT). Then we look for the NVIDIA names, which are unambiguous.
        bool matchedByName = false;
        if (views.empty()) {
            NV_CUSTOM_SEMANTIC nvidiaSemantics[2]{};
            nvidiaSemantics[0].NVCustomSemanticType = NV_X_RIGHT_SEMANTIC;
            strcpy_s(nvidiaSemantics[0].NVCustomSemanticNameString, "NV_X_RIGHT");
            nvidiaSemantics[1].NVCustomSemanticType = NV_POSITION_SEMANTIC;
            strcpy_s(nvidiaSemantics[1].NVCustomSemanticNameString, "NV_POSITION");
            views = FindViewOutputs(outputSignature, ARRAYSIZE(nvidiaSemantics), nvidiaSemantics);
            matchedByName = !views.empty();
        }
        if (views.empty()) {
            return notPatched("no view outputs");
        }

        const SignatureElement* positionElement = nullptr;
        bool hasViewportIndex = false;
        uint32_t nextRegister = 0;
        for (const auto& element : outputSignature.elements) {
            if (element.systemValue == kNamePosition) {
                positionElement = &element;
            } else if (element.systemValue == kNameViewportArrayIndex) {
                hasViewportIndex = true;
            }
            nextRegister = std::max(nextRegister, element.registerIndex + 1);
        }
        if (!positionElement) {
            return notPatched("no SV_Position output");
        }
        // A copy: we add elements to the signature below.
        const SignatureElement position = *positionElement;
        // Shader model 4 cannot output SV_ViewportArrayIndex from a vertex shader, so we upgrade shader model 4.0 and
        // 4.1 to 5.0, which is a superset of them (same instructions and encodings). The SHDR chunk becomes SHEX, as
        // with fxc. Geometry shaders are upgraded the same way, so that the AMD intrinsic is always used from shader
        // model 5.0.
        constexpr uint32_t kProgramTypeVertex = 1;
        constexpr uint32_t kProgramTypeGeometry = 2;
        const bool isGeometryShader = program.ProgramType() == kProgramTypeGeometry;
        const char* const stage = isGeometryShader ? "gs" : "vs";
        const bool isShaderModel4 =
            program.MajorVersion() == 4 && (program.MinorVersion() == 0 || program.MinorVersion() == 1);
        const bool isShaderModel5 = program.MajorVersion() == 5 && program.MinorVersion() == 0;
        if ((program.ProgramType() != kProgramTypeVertex && !isGeometryShader) || !(isShaderModel4 || isShaderModel5)) {
            return notPatched("only shader models 4.0, 4.1 and 5.0 of vertex and geometry shaders are supported");
        }

        const ViewportIndexIntrinsic& intrinsic = GetViewportIndexIntrinsic();
        if (!intrinsic.valid) {
            return notPatched("the AMD intrinsic is unavailable");
        }

        // Scan the declarations: find the temp count, where to insert our declarations, and make sure the slots
        // used by the intrinsic are free.
        size_t dclTemps = SIZE_MAX;
        size_t firstInputOutputDeclaration = SIZE_MAX;
        size_t lastOutputDeclaration = SIZE_MAX;
        size_t firstInstruction = SIZE_MAX;
        uint32_t maxOutputVertexCount = 0;
        for (size_t i = 0; i < program.instructions.size(); i++) {
            const Instruction& instruction = program.instructions[i];
            const uint32_t op = instruction.Opcode();
            if (!IsDeclaration(op)) {
                if (firstInstruction == SIZE_MAX) {
                    firstInstruction = i;
                }
                if (op == opcode::Call || op == opcode::CallC || op == opcode::Label || op == opcode::InterfaceCall) {
                    return notPatched("subroutines are not supported");
                }
                // A vertex shader would skip our epilogue. In a geometry shader, it only ends the emission early.
                if (op == opcode::RetC && !isGeometryShader) {
                    return notPatched("conditional returns are not supported");
                }
                continue;
            }

            switch (op) {
            case opcode::DclTemps:
                dclTemps = i;
                break;
            case opcode::DclMaxOutputVertexCount:
                maxOutputVertexCount = instruction.tokens.size() > 1 ? instruction.tokens[1] : 0;
                break;
            case opcode::DclSampler:
            case opcode::DclResource:
            case opcode::DclResourceRaw:
            case opcode::DclResourceStructured: {
                const uint32_t slot = instruction.tokens.size() > 2 ? instruction.tokens[2] : 0;
                if ((op == opcode::DclSampler && slot == kIntrinsicSamplerSlot) ||
                    (op != opcode::DclSampler && slot == kIntrinsicResourceSlot)) {
                    return notPatched("the shader uses a slot reserved for the AMD intrinsics");
                }
                break;
            }
            case opcode::DclOutput:
            case opcode::DclOutputSgv:
            case opcode::DclOutputSiv:
                lastOutputDeclaration = i;
                [[fallthrough]];
            default:
                if (op >= opcode::DclInput && op <= opcode::DclOutputSiv && firstInputOutputDeclaration == SIZE_MAX) {
                    firstInputOutputDeclaration = i;
                }
                break;
            }
        }
        if (firstInstruction == SIZE_MAX || lastOutputDeclaration == SIZE_MAX) {
            return notPatched("no instructions or output declarations");
        }

        // A geometry shader can output at most 1024 scalars in total (over all its vertices), and we add one per
        // vertex (SV_ViewportArrayIndex).
        if (isGeometryShader && !hasViewportIndex) {
            uint32_t outputScalars = 1;
            for (const auto& element : outputSignature.elements) {
                if (element.stream == position.stream) {
                    outputScalars += __popcnt(element.mask);
                }
            }
            if (outputScalars * maxOutputVertexCount > 1024) {
                char reason[256];
                sprintf_s(reason,
                          "%u vertices of %u scalars exceed the geometry shader output limit",
                          maxOutputVertexCount,
                          outputScalars);
                return notPatched(reason);
            }
        }

        // Allocate our temps after the shader's: the position, one per view, one for the view index, and one for the
        // position of the current view. The latter is separate from the position temp, so that the selection does not
        // change what the shader wrote (a geometry shader runs the selection for each vertex).
        const uint32_t tempBase = dclTemps != SIZE_MAX ? program.instructions[dclTemps].tokens[1] : 0;
        const uint32_t positionTemp = tempBase;
        for (size_t i = 0; i < views.size(); i++) {
            views[i].temp = tempBase + 1 + uint32_t(i);
        }
        const uint32_t viewIndexTemp = tempBase + 1 + uint32_t(views.size());
        const uint32_t selectedPositionTemp = viewIndexTemp + 1;
        const uint32_t tempCount = selectedPositionTemp + 1;

        // Redirect the writes to the position outputs to our temps (outputs cannot be read back).
        std::vector<OperandLocation> operands;
        for (auto& instruction : program.instructions) {
            if (IsDeclaration(instruction.Opcode())) {
                continue;
            }
            if (!instruction.GetOperands(operands)) {
                return notPatched("failed to decode opcode " + std::to_string(instruction.Opcode()));
            }
            for (const auto& operand : operands) {
                uint32_t& token = instruction.tokens[operand.token];
                if (OperandType(token) != operand_type::Output) {
                    continue;
                }
                if (OperandIndexDimension(token) != 1 || OperandIndexRepresentation(token, 0) != 0) {
                    return notPatched("indexed output");
                }
                uint32_t& index = instruction.tokens[operand.firstIndex];
                uint32_t temp = UINT32_MAX;
                if (index == position.registerIndex) {
                    temp = positionTemp;
                }
                for (const auto& view : views) {
                    if (index == view.registerIndex) {
                        temp = view.temp;
                    }
                }
                if (temp != UINT32_MAX) {
                    token = SetOperandType(token, operand_type::Temp);
                    index = temp;
                }
            }
        }

        // Retarget the intrinsic's sample to our view index temp.
        Instruction sample = intrinsic.sample;
        if (!sample.GetOperands(operands) || operands.empty()) {
            return notPatched("unexpected code for the AMD intrinsic");
        }
        sample.tokens[operands[0].token] = SetOperandType(sample.tokens[operands[0].token], operand_type::Temp);
        sample.tokens[operands[0].firstIndex] = viewIndexTemp;

        // The epilogue, run before each return (vertex shader) or each emit (geometry shader):
        //   r_view = AmdDxExtShaderIntrinsics_GetViewportIndex()
        //   r_selected = r_position
        //   for each view N: r_selected = (r_view.x == N) ? r_N : r_selected   (only .x for NV_X_RIGHT)
        //   o_position = r_selected, o_N = r_N, o_viewport = r_view.x
        const uint8_t viewportRegister = uint8_t(nextRegister);
        std::vector<Instruction> epilogue;
        epilogue.push_back(sample);
        epilogue.push_back(Instruction::Make(opcode::Mov,
                                             {DestinationOperand(operand_type::Temp, selectedPositionTemp, 0xf),
                                              SourceOperand(operand_type::Temp, positionTemp, kSwizzleXYZW)}));
        for (const auto& view : views) {
            epilogue.push_back(Instruction::Make(opcode::Ieq,
                                                 {DestinationOperand(operand_type::Temp, viewIndexTemp, 0x2),
                                                  SourceOperand(operand_type::Temp, viewIndexTemp, Swizzle(0, 0, 0, 0)),
                                                  ImmediateOperand(view.view)}));
            epilogue.push_back(Instruction::Make(opcode::Movc,
                                                 {DestinationOperand(operand_type::Temp, selectedPositionTemp, view.xOnly ? 0x1 : 0xf),
                                                  SourceOperand(operand_type::Temp, viewIndexTemp, Swizzle(1, 1, 1, 1)),
                                                  SourceOperand(operand_type::Temp, view.temp, kSwizzleXYZW),
                                                  SourceOperand(operand_type::Temp, selectedPositionTemp, kSwizzleXYZW)}));
        }
        epilogue.push_back(Instruction::Make(opcode::Mov,
                                             {DestinationOperand(operand_type::Output, position.registerIndex, position.mask),
                                              SourceOperand(operand_type::Temp, selectedPositionTemp, kSwizzleXYZW)}));
        for (const auto& view : views) {
            epilogue.push_back(Instruction::Make(opcode::Mov,
                                                 {DestinationOperand(operand_type::Output, view.registerIndex, view.mask),
                                                  SourceOperand(operand_type::Temp, view.temp, kSwizzleXYZW)}));
        }
        if (!hasViewportIndex) {
            epilogue.push_back(Instruction::Make(opcode::Mov,
                                                 {DestinationOperand(operand_type::Output, viewportRegister, 0x1),
                                                  SourceOperand(operand_type::Temp, viewIndexTemp, Swizzle(0, 0, 0, 0))}));
        }

        // Rebuild the program with our declarations and epilogue.
        std::vector<Instruction> instructions;
        for (size_t i = 0; i < program.instructions.size(); i++) {
            const Instruction& instruction = program.instructions[i];

            if (i == firstInputOutputDeclaration) {
                instructions.push_back(intrinsic.dclSampler);
                instructions.push_back(intrinsic.dclResource);
            }
            if (i == firstInstruction && dclTemps == SIZE_MAX) {
                Instruction declaration;
                declaration.tokens = {opcode::DclTemps | (2u << 24), tempCount};
                instructions.push_back(declaration);
            }
            const uint32_t op = instruction.Opcode();
            const bool isEmit = op == opcode::Emit || op == opcode::EmitThenCut || op == opcode::EmitStream ||
                                op == opcode::EmitThenCutStream;
            if (isGeometryShader ? isEmit : op == opcode::Ret) {
                instructions.insert(instructions.end(), epilogue.begin(), epilogue.end());
            }

            instructions.push_back(instruction);
            if (i == dclTemps) {
                instructions.back().tokens[1] = tempCount;
            }

            if (i == lastOutputDeclaration && !hasViewportIndex) {
                // dcl_output_siv o#.x, viewport_array_index
                Instruction declaration;
                declaration.tokens = DestinationOperand(operand_type::Output, viewportRegister, 0x1);
                declaration.tokens.insert(declaration.tokens.begin(), opcode::DclOutputSiv);
                declaration.tokens.push_back(kNameViewportArrayIndex);
                declaration.tokens[0] |= uint32_t(declaration.tokens.size()) << 24;
                instructions.push_back(declaration);
            }
        }
        program.instructions = std::move(instructions);

        if (!hasViewportIndex) {
            SignatureElement element;
            element.semanticName = "SV_ViewportArrayIndex";
            element.systemValue = kNameViewportArrayIndex;
            element.componentType = kComponentTypeUInt32;
            element.registerIndex = viewportRegister;
            element.mask = 0x1;
            // For outputs, the components that are never written.
            element.readWriteMask = 0;
            element.stream = position.stream;
            outputSignature.elements.push_back(element);
        }

        LOG_DETAIL("  Patched: position o%u -> r%u, view index in r%u", position.registerIndex, positionTemp, viewIndexTemp);
        if (!hasViewportIndex) {
            LOG_DETAIL(" -> o%u", viewportRegister);
        }
        LOG_DETAIL("\n");
        for (const auto& view : views) {
            LOG_DETAIL("    View %u: o%u%s -> r%u\n", view.view, view.registerIndex, view.xOnly ? " (x only)" : "", view.temp);
        }
        const bool xOnly = std::all_of(views.cbegin(), views.cend(), [](const ViewOutput& view) { return view.xOnly; });
        summary = "rewritten (" + std::to_string(views.size()) + (views.size() == 1 ? " extra view" : " extra views") +
                  (xOnly ? ", x only" : "") + (matchedByName ? ", outputs matched by name)" : ")");

        if (isShaderModel4) {
            summary += ", upgraded from " + std::string(stage) + "_" + std::to_string(program.MajorVersion()) + "_" +
                       std::to_string(program.MinorVersion());
            program.versionToken = (program.versionToken & ~0xffu) | (5u << 4);
            program.fourCC = MakeFourCC('S', 'H', 'E', 'X');
        }
        *programChunk = program.Serialize();
        *outputChunk = outputSignature.Serialize();
        return true;
    }

} // namespace

namespace shaders {

    bool ProcessShader(const char* stage,
                       const void* bytecode,
                       size_t length,
                       NvU32 numCustomSemantics,
                       const NV_CUSTOM_SEMANTIC* customSemantics,
                       std::vector<uint8_t>& output) {
        dxbc::Container container;
        if (!container.Parse(bytecode, length)) {
            logging::Log("Failed to parse %s shader\n", stage);
            return false;
        }

        const auto bytes = static_cast<const uint8_t*>(bytecode);
        const std::string hash = ShaderHash(bytes);
        std::string chunkList;
        for (const auto& chunk : container.chunks) {
            chunkList += " " + dxbc::FourCCToString(chunk.fourCC);
        }
        LOG_DETAIL("%s shader %s (%zu bytes):%s\n", stage, hash.c_str(), length, chunkList.c_str());

        for (NvU32 i = 0; i < numCustomSemantics; i++) {
            const NV_CUSTOM_SEMANTIC& semantic = customSemantics[i];
            LOG_DETAIL("  Custom semantic: %s \"%s\"", CustomSemanticTypeName(semantic.NVCustomSemanticType),
                       semantic.NVCustomSemanticNameString);
            if (semantic.RegisterSpecified) {
                LOG_DETAIL(" (reg=%u mask=0x%x)", semantic.RegisterNum, semantic.RegisterMask);
            }
            LOG_DETAIL("\n");
        }

        // Parse and re-serialize every chunk we understand.
        for (auto& chunk : container.chunks) {
            if (dxbc::Signature::IsSignatureChunk(chunk.fourCC)) {
                dxbc::Signature signature;
                if (!signature.Parse(chunk)) {
                    logging::Log("%s shader %s: failed to parse the %s chunk\n", stage, hash.c_str(), dxbc::FourCCToString(chunk.fourCC).c_str());
                    return false;
                }
                LogSignature(signature);
                chunk = signature.Serialize();
            } else if (dxbc::Program::IsProgramChunk(chunk.fourCC)) {
                dxbc::Program program;
                if (!program.Parse(chunk)) {
                    logging::Log("%s shader %s: failed to parse the %s chunk\n", stage, hash.c_str(), dxbc::FourCCToString(chunk.fourCC).c_str());
                    return false;
                }
                LOG_DETAIL("  %s: shader model %u.%u, %zu instructions\n",
                           dxbc::FourCCToString(chunk.fourCC).c_str(),
                           program.MajorVersion(),
                           program.MinorVersion(),
                           program.instructions.size());
                chunk = program.Serialize();
            }
        }

        output = container.Serialize();

        DumpDisassembly(hash + "." + stage, bytecode, length);

#if 0
        // Parser validation: when nothing was changed, the output must be identical to the input, checksum included.
        if (output.size() != length || memcmp(output.data(), bytecode, length) != 0) {
            logging::Log("  Round-trip mismatch (%zu bytes vs %zu)!\n", output.size(), length);
            dxbc::Container original;
            original.Parse(bytecode, length);
            dxbc::Container rebuilt;
            rebuilt.Parse(output.data(), output.size());
            for (size_t i = 0; i < original.chunks.size() && i < rebuilt.chunks.size(); i++) {
                if (original.chunks[i].data != rebuilt.chunks[i].data) {
                    logging::Log("  Chunk %s differs\n", dxbc::FourCCToString(original.chunks[i].fourCC).c_str());
                }
            }
            if (memcmp(output.data() + 4, bytes + 4, 16) != 0) {
                logging::Log("  Checksum differs\n");
            }
            return false;
        }
#endif

        // Fast geometry shaders are dropped (see NvAPI_D3D11_CreateGeometryShaderEx_2()).
        std::string summary = strcmp(stage, "fastgs") == 0 ? "dropped (fast geometry shader)" : "passed through unchanged";
        if ((strcmp(stage, "vs") == 0 || strcmp(stage, "gs") == 0) &&
            PatchShader(container, numCustomSemantics, customSemantics, summary)) {
            output = container.Serialize();
            DumpDisassembly(hash + "." + stage + ".patched", output.data(), output.size());
        }
        logging::Log("%s shader %s: %s\n", stage, hash.c_str(), summary.c_str());
        return true;
    }

} // namespace shaders
