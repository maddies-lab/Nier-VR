// Ranks functions by how many of the given store displacements they write
// (e.g. the CameraData matrix rows), to find the code that fills a structure.
// Usage (headless): ... -postScript FindStores.java <out-file> <disp> [<disp> ...]
// @category NieR-VR

import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.TreeSet;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.lang.OperandType;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.scalar.Scalar;

public class FindStores extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Set<Long> wanted = new HashSet<>();
        for (int i = 1; i < args.length; i++) {
            wanted.add(Long.decode(args[i]));
        }

        List<String> rows = new ArrayList<>();
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            Set<Long> hit = new TreeSet<>();
            for (Instruction insn : currentProgram.getListing().getInstructions(f.getBody(), true)) {
                if (insn.getNumOperands() < 2 || (insn.getOperandType(0) & OperandType.DYNAMIC) == 0) {
                    continue; // only stores: memory destination in operand 0
                }
                Object[] objects = insn.getOpObjects(0);
                boolean stack = false;
                for (Object o : objects) {
                    if (o instanceof Register) {
                        String name = ((Register) o).getName();
                        stack |= name.equals("RSP") || name.equals("RBP");
                    }
                }
                if (stack) {
                    continue; // locals, not a structure field
                }
                for (Object o : objects) {
                    if (o instanceof Scalar && wanted.contains(((Scalar) o).getUnsignedValue())) {
                        hit.add(((Scalar) o).getUnsignedValue());
                    }
                }
            }
            if (hit.size() >= 3) {
                StringBuilder sb = new StringBuilder();
                for (Long d : hit) {
                    sb.append(String.format(" %x", d));
                }
                rows.add(String.format("%03d %s %s :%s", hit.size(), f.getEntryPoint(), f.getName(), sb));
            }
        }

        rows.sort((a, b) -> b.compareTo(a));
        try (PrintWriter out = new PrintWriter(new File(args[0]))) {
            for (String row : rows) {
                out.println(row);
            }
        }
        println("FindStores: " + rows.size() + " functions written to " + args[0]);
    }
}
