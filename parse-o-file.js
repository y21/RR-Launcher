const fs = require("fs");
require("child_process").execSync(
  "/opt/devkitpro/devkitPPC/bin/powerpc-eabi-gcc -c x.c -O3"
);
const out = require("child_process")
  .execSync("/opt/devkitpro/devkitPPC/bin/powerpc-eabi-objdump x.o -d")
  .toString();

for (const [, name, l1, l2, l3, l4] of out.matchAll(
  /<(\w+)>:\n(.+)\n(.+)\n(.+)\n(.+)/g
)) {
  const parseLine = (x) => x.split(/\s+/g);
  const res = [l1, l2, l3, l4]
    .map((v) => "0x" + parseLine(v).slice(2, 6).join(""))
    .join(",");
  console.log(name, res);
}
// console.log(out);
