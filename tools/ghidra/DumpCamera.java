// Decompiles everything camera-related into text files for offline reading.
// Usage (headless):
//   analyzeHeadless re/ghidra NieRAutomata -process NieRAutomata.decrypted.exe -noanalysis \
//       -scriptPath tools/ghidra -postScript DumpCamera.java <out-dir> <camera-instance-rva> <camera-vftable-rva>
// @category NieR-VR

import java.io.File;
import java.io.PrintWriter;
import java.util.LinkedHashSet;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;

public class DumpCamera extends GhidraScript {
    private DecompInterface decompiler;
    private File outDir;
    private PrintWriter index;
    private final Set<Function> done = new LinkedHashSet<>();

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        outDir = new File(args[0]);
        outDir.mkdirs();
        Address base = currentProgram.getImageBase();
        Address instance = base.add(Long.decode(args[1]));
        Address vftable = base.add(Long.decode(args[2]));

        decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        index = new PrintWriter(new File(outDir, "index.txt"));

        index.println("== RTTI camera classes (vftables)");
        for (Symbol s : currentProgram.getSymbolTable().getAllSymbols(true)) {
            String path = s.getName(true);
            if (path.contains("Camera") && s.getName().contains("vftable")) {
                index.printf("%s  %s  entries=%d%n", s.getAddress(), path, countEntries(s.getAddress()));
            }
        }

        index.println("\n== cCameraGame vftable " + vftable);
        int n = countEntries(vftable);
        for (int i = 0; i < n; i++) {
            Address target = toAddr(getLong(vftable.add(8L * i)));
            Function f = getFunctionAt(target);
            index.printf("  [%d] %s %s%n", i, target, f != null ? f.getName(true) : "?");
            dump(f, 2, "vfunc" + i);
        }

        index.println("\n== Functions referencing the cCameraGame instance " + instance);
        Set<Function> users = new LinkedHashSet<>();
        for (Reference r : getReferencesTo(instance)) {
            Function f = getFunctionContaining(r.getFromAddress());
            if (f != null && users.add(f)) {
                index.printf("  %s %s (ref at %s)%n", f.getEntryPoint(), f.getName(true), r.getFromAddress());
            }
        }
        for (Function f : users) {
            dump(f, 1, "user");
        }

        index.close();
        println("Dumped " + done.size() + " functions to " + outDir);
    }

    private int countEntries(Address vftable) throws Exception {
        int n = 0;
        while (n < 512) {
            long value = getLong(vftable.add(8L * n));
            Address target;
            try {
                target = toAddr(value);
            } catch (Exception e) {
                break;
            }
            if (getFunctionAt(target) == null) {
                break;
            }
            n++;
        }
        return n;
    }

    private void dump(Function f, int depth, String tag) throws Exception {
        if (f == null || depth < 0 || !done.add(f)) {
            return;
        }
        DecompileResults result = decompiler.decompileFunction(f, 60, monitor);
        String name = f.getEntryPoint().toString() + "_" + tag + ".c";
        try (PrintWriter out = new PrintWriter(new File(outDir, name))) {
            out.println("// " + f.getName(true) + " @ " + f.getEntryPoint());
            out.println(result.decompileCompleted() ? result.getDecompiledFunction().getC() : "// decompile failed");
        }
        for (Function callee : f.getCalledFunctions(monitor)) {
            dump(callee, depth - 1, tag + "-callee");
        }
    }
}
