function bag() {
  if (!globalThis.__apexoFake) {
    globalThis.__apexoFake = { accepts: [], vad: [], models: [], prompts: [], deleted: 0, creates: 0, resets: 0 };
  }
  return globalThis.__apexoFake;
}

export class Engine {
  static async create(options) {
    bag().models.push(options.model);
    return new Engine();
  }

  async createConversation() {
    return {
      async sendMessage(prompt) {
        bag().prompts.push(prompt);
        return { content: [{ text: "适配器摘要" }] };
      },
    };
  }

  delete() {
    bag().deleted += 1;
  }
}
