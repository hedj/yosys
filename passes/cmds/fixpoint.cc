/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/yosys.h"
#include "kernel/sigtools.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

// A cheap structural signature of the selected design. Two designs with
// different signatures certainly differ; equal signatures are taken to mean
// the last iteration changed nothing.
static Hasher::hash_t design_signature(RTLIL::Design *design)
{
	Hasher h;
	for (auto module : design->selected_modules()) {
		h.eat(module->name);
		h.eat(GetSize(module->wires()));
		h.eat(GetSize(module->cells()));
		h.eat(GetSize(module->connections()));
		std::vector<Hasher::hash_t> cell_hashes;
		for (auto cell : module->cells()) {
			Hasher ch;
			ch.eat(cell->type);
			for (auto &param : cell->parameters) {
				ch.eat(param.first);
				ch.eat(param.second);
			}
			for (auto &conn : cell->connections()) {
				ch.eat(conn.first);
				ch.eat(GetSize(conn.second));
				int const_bits = 0;
				for (auto bit : conn.second)
					if (bit.wire == nullptr)
						const_bits++;
				ch.eat(const_bits);
			}
			cell_hashes.push_back(ch.yield());
		}
		std::sort(cell_hashes.begin(), cell_hashes.end());
		for (auto ch : cell_hashes)
			h.eat(ch);
	}
	return h.yield();
}

struct FixpointPass : public Pass {
	FixpointPass() : Pass("fixpoint", "run commands repeatedly until the design stops changing") { }
	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    fixpoint [-max <n>] \"<commands>\"\n");
		log("\n");
		log("Run the given commands (separated by ';') repeatedly until an iteration leaves\n");
		log("the design structurally unchanged, or <n> iterations have run (default 10).\n");
		log("\n");
		log("This lets a flow apply a group of optimisations that enable each other, for\n");
		log("example all word-level optimisations, without fixing their relative order.\n");
		log("\n");
	}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		int max_iter = 10;
		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			if (args[argidx] == "-max" && argidx + 1 < args.size()) {
				max_iter = std::stoi(args[++argidx]);
				continue;
			}
			break;
		}
		if (argidx + 1 != args.size())
			log_cmd_error("Expected exactly one (quoted) command string.\n");
		std::string commands = args[argidx];
		if (commands.size() >= 2 && commands.front() == '"' && commands.back() == '"')
			commands = commands.substr(1, commands.size() - 2);

		log_header(design, "Executing FIXPOINT pass.\n");
		log_push();
		auto sig = design_signature(design);
		int iter;
		for (iter = 1; iter <= max_iter; iter++) {
			log_header(design, "Fixpoint iteration %d.\n", iter);
			Pass::call(design, commands);
			auto new_sig = design_signature(design);
			if (new_sig == sig)
				break;
			sig = new_sig;
		}
		log_pop();
		if (iter > max_iter)
			log_warning("fixpoint: no fixed point after %d iterations.\n", max_iter);
		else
			log("Reached a fixed point after %d iteration(s).\n", iter);
	}
} FixpointPass;

static std::string unquote(std::string s)
{
	if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
		return s.substr(1, s.size() - 2);
	return s;
}

static int count_cells(RTLIL::Design *design)
{
	int n = 0;
	for (auto module : design->modules())
		for (auto cell : module->cells())
			if (cell->type != ID($scopeinfo))
				n++;
	return n;
}

struct BestofPass : public Pass {
	BestofPass() : Pass("bestof", "try alternative command sequences and keep the best result") { }
	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    bestof [-cost \"<commands>\"] \"<alternative>\" \"<alternative>\" ...\n");
		log("\n");
		log("Run each alternative (a ';'-separated command list) on a copy of the current\n");
		log("design, then keep the result of the cheapest one. Alternatives that produce\n");
		log("structurally identical designs are only costed once.\n");
		log("\n");
		log("The cost of a result is its cell count after running the cost commands on a\n");
		log("further copy of it, so that choices are judged by what they lead to after\n");
		log("lowering rather than by the cell count at the current level.\n");
		log("\n");
		log("    -cost \"<commands>\"\n");
		log("        default: \"techmap; opt -fast; abc; opt -fast\"\n");
		log("\n");
		log("Ties go to the earliest alternative.\n");
		log("\n");
		log("Caveat: several passes still order objects by IdString index (their position\n");
		log("in the global name table), so the cost runs can still influence what later\n");
		log("passes produce. See kernel/natural_order.h.\n");
		log("\n");
	}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		std::string cost_script = "techmap; opt -fast; abc; opt -fast";
		std::vector<std::string> alternatives;
		for (size_t argidx = 1; argidx < args.size(); argidx++) {
			if (args[argidx] == "-cost" && argidx + 1 < args.size()) {
				cost_script = unquote(args[++argidx]);
				continue;
			}
			alternatives.push_back(unquote(args[argidx]));
		}
		if (alternatives.empty())
			log_cmd_error("No alternatives given.\n");

		log_header(design, "Executing BESTOF pass.\n");
		log_push();
		Pass::call(design, "design -save __bestof_base");

		dict<Hasher::hash_t, int> seen; // design signature -> alternative index
		std::vector<int> costs(alternatives.size(), -1);
		int best = -1;
		for (int i = 0; i < GetSize(alternatives); i++) {
			log_header(design, "Alternative %d: %s\n", i + 1, alternatives[i]);
			Pass::call(design, "design -load __bestof_base");
			Pass::call(design, alternatives[i]);
			auto sig = design_signature(design);
			if (seen.count(sig)) {
				costs[i] = costs[seen.at(sig)];
				log("Same result as alternative %d.\n", seen.at(sig) + 1);
				continue;
			}
			seen[sig] = i;
			Pass::call(design, stringf("design -save __bestof_%d", i));
			Pass::call(design, cost_script);
			costs[i] = count_cells(design);
			if (best < 0 || costs[i] < costs[best])
				best = i;
		}

		Pass::call(design, stringf("design -load __bestof_%d", best));
		Pass::call(design, "design -delete __bestof_base");
		for (auto &it : seen)
			Pass::call(design, stringf("design -delete __bestof_%d", it.second));
		log_pop();

		for (int i = 0; i < GetSize(alternatives); i++)
			log("  %s cost %6d  %s\n", i == best ? "*" : " ", costs[i], alternatives[i]);
		log("Kept alternative %d.\n", best + 1);
	}
} BestofPass;

PRIVATE_NAMESPACE_END
