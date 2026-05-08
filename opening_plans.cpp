#include "opening_plans.h"

#include <utility>
#include <vector>

namespace {

// Match table — first row whose key is a prefix of the opening
// name wins, so the family-level entries (e.g. "Sicilian Defense")
// catch every variation that doesn't have a more specific plan
// listed above. Keep more-specific entries above their family.
const std::vector<std::pair<std::string, std::string>>& plans() {
    static const std::vector<std::pair<std::string, std::string>> table = {
        // ----- Sicilian Defense — variations and family.
        {"Sicilian Defense: Najdorf Variation",
         "Black plans …e5 or …e6 with queenside expansion via …b5 "
         "and pressure on e4. White typically goes English Attack "
         "with f3, Be3, Qd2, O-O-O, and a kingside pawn storm."},
        {"Sicilian Defense: Dragon Variation",
         "Black fianchettos the king bishop and aims at e4 / "
         "queenside files. White's main plan is Yugoslav Attack "
         "with O-O-O and h-pawn rushes."},
        {"Sicilian Defense: Sveshnikov",
         "Black accepts a backward d-pawn for active piece play. "
         "White tries to clamp d5 and exploit the hole."},
        {"Sicilian Defense: Scheveningen",
         "Both sides race; white's English Attack vs black's …b5 "
         "expansion is the classical battle."},
        {"Sicilian Defense: Taimanov",
         "Black keeps the centre flexible with …a6 and …Qc7, "
         "delaying …Nf6 to dodge the Maroczy bind."},
        {"Sicilian Defense",
         "Black gains queenside space and chases the e-file. "
         "White's typical plan is to support e4, push f3 / Be3 / "
         "Qd2 and storm the king with g4 / h4."},

        // ----- Ruy Lopez (Spanish).
        {"Ruy Lopez: Berlin Defense",
         "Black accepts a slightly worse endgame for piece "
         "activity and king safety. White grinds with the bishop "
         "pair and central control."},
        {"Ruy Lopez: Closed",
         "Slow manoeuvring; white's plan is Nbd2-Nf1-Ng3 and "
         "central / kingside expansion, black aims at …d5 or …f5."},
        {"Ruy Lopez",
         "White pressures the e5 pawn and the c6-knight; "
         "common plans are c3-d4 in the centre or queenside "
         "expansion with a4."},

        // ----- Italian / Giuoco.
        {"Italian Game: Evans Gambit",
         "White sacrifices b4 to gain time for c3 + d4 and a "
         "central blast against the black king."},
        {"Italian Game",
         "Slow-burn — white plans c3 + d4 to claim the centre, "
         "black holds with …d6 and …Bg4 / …h6 hooks."},

        // ----- French.
        {"French Defense: Advance",
         "Locked centre — both sides expand on the wings; black "
         "attacks the d4 base with …c5 and …Nc6, white pushes "
         "kingside."},
        {"French Defense: Winawer",
         "Black trades the dark-squared bishop for white's c3-"
         "knight and aims for …c5 plus queenside play; white "
         "tries to mate on the kingside."},
        {"French Defense",
         "Black's structure constrains the king bishop; standard "
         "plan is …c5 to challenge d4 and …f6 to break the e5 / "
         "d4 pawn chain."},

        // ----- Caro-Kann.
        {"Caro-Kann Defense: Advance",
         "Black plays …Bf5 / …e6 / …c5 to undermine d4; white "
         "goes for kingside space with h4 or central play with "
         "Nf3 / Be2 / O-O."},
        {"Caro-Kann Defense",
         "Solid; black trades light-squared bishops early and "
         "aims for a sound endgame. White's plan is to claim space "
         "in the centre and outpost on e5."},

        // ----- King's Indian / Pirc / Modern.
        {"King's Indian Defense",
         "Locked centre — black storms the kingside with …f5 / "
         "…g4 while white expands queenside with c5, b4, a4 in a "
         "race for the opponent's king."},
        {"Pirc Defense",
         "Black fianchettos and lets white build a big centre, "
         "then breaks with …c5 / …e5. White goes Austrian Attack "
         "(f4) for a kingside crush."},
        {"Modern Defense",
         "Hyper-flexible; black delays committing pieces and aims "
         "to challenge the centre with …c5 or …e5 once white has "
         "shown his hand."},

        // ----- Queen's Gambit.
        {"Queen's Gambit Declined",
         "White claims the centre and pressures the half-open "
         "c-file; black aims for …c5 or the Carlsbad minority "
         "attack on the queenside."},
        {"Queen's Gambit Accepted",
         "White gets a free hand in the centre with e4; black "
         "tries to chip back at d4 with …c5 and develop quickly."},

        // ----- Slav.
        {"Slav Defense",
         "Black holds the d5 pawn with …c6 and develops the "
         "light-squared bishop outside the chain. White plays "
         "for a small but lasting space advantage."},

        // ----- Indian / Nimzo / Queen's Indian.
        {"Nimzo-Indian Defense",
         "Black gives up the bishop pair for doubled c-pawns "
         "and central pressure. White's plan is to unblock with "
         "a3 / e3 / Bd3 and exploit the bishops in the long run."},
        {"Queen's Indian Defense",
         "Black fianchettos both bishops and contests the long "
         "diagonals. White aims for d5 / c4 expansion and tries "
         "to chase the Indian setup from its grip."},
        {"Grünfeld Defense",
         "Black surrenders the centre, then attacks white's pawn "
         "centre with …c5, …Bg7, and queenside pressure."},
        {"Catalan Opening",
         "White's Bg2 looks down the long diagonal at the "
         "queenside; the typical plan is to recapture the c-pawn "
         "with Qa4 or a4 / Qa4 and squeeze with central control."},

        // ----- English / Reti.
        {"English Opening",
         "White claims the long diagonal and the c-file; common "
         "plans are e4 reverse-Sicilian setups or d4 transposing "
         "into a Queen's pawn opening."},
        {"Réti Opening",
         "White hyper-modernises — fianchettos the king bishop, "
         "delays pawn moves, and pressures black's centre from "
         "the wings."},

        // ----- King's Pawn (1.e4 e5) sidelines.
        {"Petrov's Defense",
         "Black mirrors white and aims for symmetric exchanges; "
         "white's edge is the move, so the plan is to keep tension "
         "and avoid trades."},
        {"Scotch Game",
         "White breaks the centre early with d4 and gets open "
         "lines; black aims for active piece play and equality."},
        {"Vienna Game",
         "White prepares an early f4 / d4 push for a kingside "
         "attack; black breaks symmetrically with …d5."},
    };
    return table;
}

}  // namespace

std::string opening_plan_for(const std::string& opening_name) {
    if (opening_name.empty()) return "";
    for (const auto& [key, plan] : plans()) {
        if (opening_name.size() >= key.size() &&
            opening_name.compare(0, key.size(), key) == 0) {
            return plan;
        }
    }
    return "";
}
