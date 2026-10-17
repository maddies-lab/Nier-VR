// Prints the caller tree of functions (who calls whom, upwards) to a text file.
// Usage (headless): ... -postScript CallerTree.java <out-file> <depth> <address> [<address> ...]
// @category NieR-VR

import java.io.File;
import java.io.PrintWriter;
import java.util.HashSet;
import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class CallerTree extends GhidraScript {
    private PrintWriter out;

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        int depth = Integer.parseInt(args[1]);
        try (PrintWriter writer = new PrintWriter(new File(args[0]))) {
            out = writer;
            for (int i = 2; i < args.length; i++) {
                Function f = getFunctionContaining(toAddr(Long.decode(args[i])));
                if (f != null) {
                    walk(f, depth, 0, new HashSet<>());
                }
                out.println();
            }
        }
        println("CallerTree written to " + args[0]);
    }

    private void walk(Function f, int depth, int level, Set<Function> path) throws Exception {
        String indent = "  ".repeat(level);
        long size = f.getBody().getNumAddresses();
        out.printf("%s%s (%d bytes)%n", indent, f.getEntryPoint(), size);
        if (level >= depth || !path.add(f)) {
            return;
        }
        int shown = 0;
        for (Reference r : getReferencesTo(f.getEntryPoint())) {
            if (++shown > 12) {
                out.printf("%s  ...%n", indent);
                break;
            }
            Function caller = getFunctionContaining(r.getFromAddress());
            if (caller == null) {
                out.printf("%s  [%s from %s]%n", indent, r.getReferenceType(), r.getFromAddress());
                continue;
            }
            walk(caller, depth, level + 1, path);
        }
        path.remove(f);
    }
}
