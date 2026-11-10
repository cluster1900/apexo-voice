export function mixChannels(channels) {
  if (!channels || !channels.length || !channels[0] || !channels[0].length) return null;
  const length = channels[0].length;
  const mixed = new Float32Array(length);
  let used = 0;
  for (const channel of channels) {
    if (!channel || channel.length !== length) continue;
    for (let i = 0; i < length; i += 1) mixed[i] += channel[i];
    used += 1;
  }
  if (!used) return null;
  if (used === 1) return mixed;
  for (let i = 0; i < length; i += 1) mixed[i] /= used;
  return mixed;
}
