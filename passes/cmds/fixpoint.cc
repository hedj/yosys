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

PRIVATE_NAMESPACE_END
