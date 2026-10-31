#include "engine.h"
namespace zw { std::unique_ptr<Engine> makeRealEngine(Loop&, IpcServer&, const RealConfig&){ throw std::runtime_error("the real engine is not built yet; use --fake"); } }
