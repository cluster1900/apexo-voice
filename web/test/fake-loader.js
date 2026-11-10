export async function resolve(specifier, context, nextResolve) {
  if (specifier === "sherpa-onnx") {
    return { shortCircuit: true, url: new URL("./fakes/sherpa-onnx.js", import.meta.url).href };
  }
  if (specifier === "@litert-lm/core") {
    return { shortCircuit: true, url: new URL("./fakes/litert-lm.js", import.meta.url).href };
  }
  return nextResolve(specifier, context);
}
