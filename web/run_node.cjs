// Run wasm checks against host files without copying the game installation into memory.
const path = require('node:path');
const [script, ...args] = process.argv.slice(2);
if (!script) throw new Error('Expected an Emscripten test module');
const cwd = process.cwd();
const windows = process.platform === 'win32';
const virtualPath = value => {
    if (windows && /^[A-Za-z]:[\\/]/.test(value))
        return '/host/' + value[0].toUpperCase() + value.slice(2).replaceAll('\\', '/');
    if (!windows && path.isAbsolute(value)) return '/host' + value;
    return value;
};
const roots = new Set([cwd, ...args].filter(value => /^[A-Za-z]:[\\/]/.test(value))
    .map(value => value[0].toUpperCase()));
require(path.resolve(script))({
    arguments: args.map(virtualPath),
    preRun: [module => {
        const {FS, NODEFS} = module;
        FS.mkdir('/host');
        if (windows) {
            for (const drive of roots) {
                FS.mkdir('/host/' + drive);
                FS.mount(NODEFS, {root: drive + ':/'}, '/host/' + drive);
            }
        } else {
            FS.mount(NODEFS, {root: '/'}, '/host');
        }
        FS.chdir(virtualPath(cwd));
    }],
}).catch(error => {
    console.error(error);
    process.exitCode = 1;
});
