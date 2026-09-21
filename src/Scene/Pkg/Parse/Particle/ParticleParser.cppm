module;

export module wescene.pkg.parse:particle_parser;
import rstd;
import wescene.json;
import wescene.scene;
import wescene.fs;
import wescene.particle.program;
import :particle_runtime;

export import wescene.pkg.scene_obj;

export namespace owe

{
class ParticleParser {
public:
    static ParticleSpawnInstruction GenInitializer(const Json&, u32 implicit_sequence_count);
    static Box<dyn<particle::ParticleUpdateProgram>>
    GenOperator(const Json&, ParticleInstanceModifiers, ParticleSubSystem&, usize operator_index);
    static Box<dyn<particle::ParticleEmitterProgram>> GenEmitter(const wpscene::Emitter&,
                                                                 ParticleSubSystem&, usize);
    static ParticleSpawnInstruction                   GenOverride(ParticleInstanceModifiers);
};
} // namespace owe
