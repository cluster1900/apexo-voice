import { register } from "node:module";

register("./fake-loader.js", import.meta.url);
