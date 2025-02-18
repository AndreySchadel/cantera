/**
 *  @file Reaction.cpp
 */

// This file is part of Cantera. See License.txt in the top-level directory or
// at https://cantera.org/license.txt for license and copyright information.

#include "cantera/kinetics/Reaction.h"
#include "ReactionFactory.h"
#include "cantera/kinetics/ReactionRateFactory.h"
#include "cantera/kinetics/Kinetics.h"
#include "cantera/thermo/ThermoPhase.h"
#include "cantera/thermo/SurfPhase.h"
#include "cantera/base/Array.h"
#include "cantera/base/AnyMap.h"
#include "cantera/base/utilities.h"
#include "cantera/base/stringUtils.h"
#include <boost/algorithm/string/predicate.hpp>
#include <sstream>
#include <set>

#include <boost/algorithm/string.hpp>

namespace ba = boost::algorithm;

namespace Cantera
{

Reaction::Reaction()
    : reversible(true)
    , duplicate(false)
    , allow_nonreactant_orders(false)
    , allow_negative_orders(false)
    , rate_units(0.0)
    , m_valid(true)
{
}

Reaction::Reaction(const Composition& reactants_,
                   const Composition& products_,
                   shared_ptr<ReactionRate> rate_)
    : reactants(reactants_)
    , products(products_)
    , reversible(true)
    , duplicate(false)
    , allow_nonreactant_orders(false)
    , allow_negative_orders(false)
    , rate_units(0.0)
    , m_valid(true)
    , m_rate(rate_)
{
}

Reaction::Reaction(const AnyMap& node, const Kinetics& kin)
    : Reaction()
{
    setParameters(node, kin);
    if (kin.nPhases()) {
        size_t nDim = kin.thermo(kin.reactionPhaseIndex()).nDim();
        if (nDim == 3) {
            setRate(newReactionRate(node, calculateRateCoeffUnits(kin)));
        } else {
            AnyMap rateNode = node;
            if (!rateNode.hasKey("type")) {
                // Reaction type is not specified
                rateNode["type"] = "Arrhenius";
            }
            std::string type = rateNode["type"].asString();
            if (rateNode.hasKey("rate-constant")) {
                if (!boost::algorithm::starts_with(type, "interface-")) {
                    rateNode["type"] = "interface-" + type;
                }
            } else if (node.hasKey("sticking-coefficient")) {
                if (!boost::algorithm::starts_with(type, "sticking-")) {
                    rateNode["type"] = "sticking-" + type;
                }
            } else {
                throw InputFileError("Reaction::Reaction", input,
                    "Unable to infer interface reaction type.");
            }
            setRate(newReactionRate(rateNode, calculateRateCoeffUnits(kin)));
        }
    } else {
        // This route is used by the Python API.
        setRate(newReactionRate(node));
    }
    check();
}

void Reaction::check()
{
    if (!allow_nonreactant_orders) {
        for (const auto& order : orders) {
            if (reactants.find(order.first) == reactants.end()) {
                throw InputFileError("Reaction::validate", input,
                    "Reaction order specified for non-reactant species '{}'",
                    order.first);
           }
        }
    }

    if (!allow_negative_orders) {
        for (const auto& order : orders) {
            if (order.second < 0.0) {
                throw InputFileError("Reaction::validate", input,
                    "Negative reaction order specified for species '{}'",
                    order.first);
            }
        }
    }

    // If reaction orders are specified, then this reaction does not follow
    // mass-action kinetics, and is not an elementary reaction. So check that it
    // is not reversible, since computing the reverse rate from thermochemistry
    // only works for elementary reactions.
    if (reversible && !orders.empty()) {
        throw InputFileError("Reaction::validate", input,
            "Reaction orders may only be given for irreversible reactions");
    }

    // Check reaction rate evaluator to ensure changes introduced after object
    // instantiation are considered.
    m_rate->check(equation(), input);
}

AnyMap Reaction::parameters(bool withInput) const
{
    AnyMap out;
    getParameters(out);
    if (withInput) {
        out.update(input);
    }

    static bool reg = AnyMap::addOrderingRules("Reaction",
        {{"head", "type"},
         {"head", "equation"},
         {"tail", "duplicate"},
         {"tail", "orders"},
         {"tail", "negative-orders"},
         {"tail", "nonreactant-orders"}
        });
    if (reg) {
        out["__type__"] = "Reaction";
    }
    return out;
}

void Reaction::getParameters(AnyMap& reactionNode) const
{
    reactionNode["equation"] = equation();

    if (duplicate) {
        reactionNode["duplicate"] = true;
    }
    if (orders.size()) {
        reactionNode["orders"] = orders;
    }
    if (allow_negative_orders) {
        reactionNode["negative-orders"] = true;
    }
    if (allow_nonreactant_orders) {
        reactionNode["nonreactant-orders"] = true;
    }

    if (m_rate) {
        reactionNode.update(m_rate->parameters());

        // strip information not needed for reconstruction
        if (reactionNode.hasKey("type")) {
            std::string type = reactionNode["type"].asString();
            if (boost::algorithm::starts_with(type, "Arrhenius")) {
                reactionNode.erase("type");
            } else if (boost::algorithm::starts_with(type, "Blowers-Masel")) {
                reactionNode["type"] = "Blowers-Masel";
            }
        }
    }
}

void Reaction::setParameters(const AnyMap& node, const Kinetics& kin)
{
    if (node.empty()) {
        // empty node: used by newReaction() factory loader
        return;
    }

    input = node;
    input.copyMetadata(node);
    setEquation(node["equation"].asString(), &kin);
    // Non-stoichiometric reaction orders
    if (node.hasKey("orders")) {
        for (const auto& order : node["orders"].asMap<double>()) {
            orders[order.first] = order.second;
            if (kin.kineticsSpeciesIndex(order.first) == npos) {
                setValid(false);
            }
        }
    }

    // Flags
    id = node.getString("id", "");
    duplicate = node.getBool("duplicate", false);
    allow_negative_orders = node.getBool("negative-orders", false);
    allow_nonreactant_orders = node.getBool("nonreactant-orders", false);
}

void Reaction::setRate(shared_ptr<ReactionRate> rate)
{
    if (!rate) {
        // null pointer
        m_rate.reset();
    } else {
        m_rate = rate;
    }

    if (reactants.count("(+M)") && std::dynamic_pointer_cast<ChebyshevRate>(m_rate)) {
        warn_deprecated("Chebyshev reaction equation", input, "Specifying '(+M)' "
            "in the reaction equation for Chebyshev reactions is deprecated.");
        // remove optional third body notation
        reactants.erase("(+M)");
        products.erase("(+M)");
    }

    if (reactants.count("M") && std::dynamic_pointer_cast<PlogRate>(m_rate)) {
        throw InputFileError("Reaction::setRate", input, "Found superfluous 'M' in "
            "pressure-dependent-Arrhenius reaction.");
    }
}

std::string Reaction::reactantString() const
{
    std::ostringstream result;
    for (auto iter = reactants.begin(); iter != reactants.end(); ++iter) {
        if (iter != reactants.begin()) {
            result << " + ";
        }
        if (iter->second != 1.0) {
            result << iter->second << " ";
        }
        result << iter->first;
    }
  return result.str();
}

std::string Reaction::productString() const
{
    std::ostringstream result;
    for (auto iter = products.begin(); iter != products.end(); ++iter) {
        if (iter != products.begin()) {
            result << " + ";
        }
        if (iter->second != 1.0) {
            result << iter->second << " ";
        }
        result << iter->first;
    }
  return result.str();
}

std::string Reaction::equation() const
{
    if (reversible) {
        return reactantString() + " <=> " + productString();
    } else {
        return reactantString() + " => " + productString();
    }
}

void Reaction::setEquation(const std::string& equation, const Kinetics* kin)
{
    parseReactionEquation(*this, equation, input, kin);
}

std::string Reaction::type() const
{
    return "reaction";
}

UnitStack Reaction::calculateRateCoeffUnits(const Kinetics& kin)
{
    if (!valid()) {
        // If a reaction is invalid because of missing species in the Kinetics
        // object, determining the units of the rate coefficient is impossible.
        return UnitStack({});
    }

    // Determine the units of the rate coefficient
    const auto& rxn_phase = kin.thermo(kin.reactionPhaseIndex());
    UnitStack rate_units(rxn_phase.standardConcentrationUnits());

    // Set output units to standardConcentrationUnits per second
    rate_units.join(1.);
    rate_units.update(Units(1.0, 0, 0, -1), 1.);

    for (const auto& order : orders) {
        const auto& phase = kin.speciesPhase(order.first);
        // Account for specified reaction orders
        rate_units.update(phase.standardConcentrationUnits(), -order.second);
    }
    for (const auto& stoich : reactants) {
        // Order for each reactant is the reactant stoichiometric coefficient,
        // unless already overridden by user-specified orders
        if (stoich.first == "M" || ba::starts_with(stoich.first, "(+")) {
            // calculateRateCoeffUnits may be called before these pseudo-species
            // have been stripped from the reactants
            continue;
        } else if (orders.find(stoich.first) == orders.end()) {
            const auto& phase = kin.speciesPhase(stoich.first);
            // Account for each reactant species
            rate_units.update(phase.standardConcentrationUnits(), -stoich.second);
        }
    }

    if (m_third_body) {
        // Account for third-body collision partner as the last entry
        rate_units.join(-1);
    }

    return rate_units;
}

void updateUndeclared(std::vector<std::string>& undeclared,
                      const Composition& comp, const Kinetics& kin)
{
    for (const auto& sp: comp) {
        if (kin.kineticsSpeciesIndex(sp.first) == npos) {
            undeclared.emplace_back(sp.first);
        }
    }
}

std::pair<std::vector<std::string>, bool> Reaction::undeclaredThirdBodies(
        const Kinetics& kin) const
{
    std::vector<std::string> undeclared;
    if (m_third_body) {
        updateUndeclared(undeclared, m_third_body->efficiencies, kin);
        return std::make_pair(undeclared, m_third_body->specified_collision_partner);
    }
    return std::make_pair(undeclared, false);
}

void Reaction::checkBalance(const Kinetics& kin) const
{
    Composition balr, balp;

    // iterate over products and reactants
    for (const auto& sp : products) {
        const ThermoPhase& ph = kin.speciesPhase(sp.first);
        size_t k = ph.speciesIndex(sp.first);
        double stoich = sp.second;
        for (size_t m = 0; m < ph.nElements(); m++) {
            balr[ph.elementName(m)] = 0.0; // so that balr contains all species
            balp[ph.elementName(m)] += stoich * ph.nAtoms(k, m);
        }
    }
    for (const auto& sp : reactants) {
        const ThermoPhase& ph = kin.speciesPhase(sp.first);
        size_t k = ph.speciesIndex(sp.first);
        double stoich = sp.second;
        for (size_t m = 0; m < ph.nElements(); m++) {
            balr[ph.elementName(m)] += stoich * ph.nAtoms(k, m);
        }
    }

    std::string msg;
    bool ok = true;
    for (const auto& el : balr) {
        const std::string& elem = el.first;
        double elemsum = balr[elem] + balp[elem];
        double elemdiff = fabs(balp[elem] - balr[elem]);
        if (elemsum > 0.0 && elemdiff / elemsum > 1e-4) {
            ok = false;
            msg += fmt::format("  {}           {}           {}\n",
                               elem, balr[elem], balp[elem]);
        }
    }
    if (!ok) {
        throw InputFileError("Reaction::checkBalance", input,
            "The following reaction is unbalanced: {}\n"
            "  Element    Reactants    Products\n{}",
            equation(), msg);
    }

    if (kin.thermo(kin.reactionPhaseIndex()).nDim() == 3) {
        return;
    }

    // Check that the number of surface sites is balanced
    double reac_sites = 0.0;
    double prod_sites = 0.0;
    auto& surf = dynamic_cast<const SurfPhase&>(kin.thermo(kin.surfacePhaseIndex()));
    for (const auto& reactant : reactants) {
        size_t k = surf.speciesIndex(reactant.first);
        if (k != npos) {
            reac_sites += reactant.second * surf.size(k);
        }
    }
    for (const auto& product : products) {
        size_t k = surf.speciesIndex(product.first);
        if (k != npos) {
            prod_sites += product.second * surf.size(k);
        }
    }
    if (fabs(reac_sites - prod_sites) > 1e-5 * (reac_sites + prod_sites)) {
        throw InputFileError("Reaction::checkBalance", input,
            "Number of surface sites not balanced in reaction {}.\n"
            "Reactant sites: {}\nProduct sites: {}",
            equation(), reac_sites, prod_sites);
    }
}

bool Reaction::checkSpecies(const Kinetics& kin) const
{
    // Check for undeclared species
    std::vector<std::string> undeclared;
    updateUndeclared(undeclared, reactants, kin);
    updateUndeclared(undeclared, products, kin);
    if (!undeclared.empty()) {
        if (kin.skipUndeclaredSpecies()) {
            return false;
        } else {
            throw InputFileError("Reaction::checkSpecies", input, "Reaction '{}'\n"
                "contains undeclared species: '{}'",
                equation(), boost::algorithm::join(undeclared, "', '"));
        }
    }

    undeclared.clear();
    updateUndeclared(undeclared, orders, kin);
    if (!undeclared.empty()) {
        if (kin.skipUndeclaredSpecies()) {
            return false;
        } else {
            if (input.hasKey("orders")) {
                throw InputFileError("Reaction::checkSpecies", input["orders"],
                    "Reaction '{}'\n"
                    "defines reaction orders for undeclared species: '{}'",
                    equation(), boost::algorithm::join(undeclared, "', '"));
            }
            // Error for empty input AnyMap (that is, XML)
            throw InputFileError("Reaction::checkSpecies", input, "Reaction '{}'\n"
                "defines reaction orders for undeclared species: '{}'",
                equation(), boost::algorithm::join(undeclared, "', '"));
        }
    }

    // Use helper function while there is no uniform handling of third bodies
    auto third = undeclaredThirdBodies(kin);
    undeclared = third.first;
    bool specified_collision_partner_ = third.second;
    if (!undeclared.empty()) {
        if (!kin.skipUndeclaredThirdBodies()) {
            if (input.hasKey("efficiencies")) {
                throw InputFileError("Reaction::checkSpecies", input["efficiencies"],
                    "Reaction '{}'\n"
                    "defines third-body efficiencies for undeclared species: '{}'",
                    equation(), boost::algorithm::join(undeclared, "', '"));
            }
            // Error for specified ThirdBody or empty input AnyMap
            throw InputFileError("Reaction::checkSpecies", input, "Reaction '{}'\n"
                "is a three-body reaction with undeclared species: '{}'",
                equation(), boost::algorithm::join(undeclared, "', '"));
        } else if (kin.skipUndeclaredSpecies() && specified_collision_partner_) {
            return false;
        }
    }

    checkBalance(kin);

    return true;
}

bool Reaction::usesElectrochemistry(const Kinetics& kin) const
{
    // Check electrochemistry
    vector_fp e_counter(kin.nPhases(), 0.0);

    // Find the number of electrons in the products for each phase
    for (const auto& sp : products) {
        size_t kkin = kin.kineticsSpeciesIndex(sp.first);
        size_t i = kin.speciesPhaseIndex(kkin);
        size_t kphase = kin.thermo(i).speciesIndex(sp.first);
        e_counter[i] += sp.second * kin.thermo(i).charge(kphase);
    }

    // Subtract the number of electrons in the reactants for each phase
    for (const auto& sp : reactants) {
        size_t kkin = kin.kineticsSpeciesIndex(sp.first);
        size_t i = kin.speciesPhaseIndex(kkin);
        size_t kphase = kin.thermo(i).speciesIndex(sp.first);
        e_counter[i] -= sp.second * kin.thermo(i).charge(kphase);
    }

    // If the electrons change phases then the reaction is electrochemical
    for (double delta_e : e_counter) {
        if (std::abs(delta_e) > 1e-4) {
            return true;
        }
    }

    return false;
}

ThirdBody::ThirdBody(double default_eff)
    : default_efficiency(default_eff)
    , specified_collision_partner(false)
    , mass_action(true)
{
}

ThirdBody::ThirdBody(const AnyMap& node)
    : specified_collision_partner(false)
    , mass_action(true)
{
    setEfficiencies(node);
}

void ThirdBody::setEfficiencies(const AnyMap& node)
{
    default_efficiency = node.getDouble("default-efficiency", 1.0);
    if (node.hasKey("efficiencies")) {
        efficiencies = node["efficiencies"].asMap<double>();
    }
}

double ThirdBody::efficiency(const std::string& k) const
{
    return getValue(efficiencies, k, default_efficiency);
}

ThreeBodyReaction::ThreeBodyReaction()
{
    m_third_body.reset(new ThirdBody);
    setRate(newReactionRate(type()));
}

ThreeBodyReaction::ThreeBodyReaction(const Composition& reactants,
                                     const Composition& products,
                                     const ArrheniusRate& rate,
                                     const ThirdBody& tbody)
    : Reaction(reactants, products, make_shared<ArrheniusRate>(rate))
{
    m_third_body = std::make_shared<ThirdBody>(tbody);
}

ThreeBodyReaction::ThreeBodyReaction(const AnyMap& node, const Kinetics& kin)
{
    m_third_body.reset(new ThirdBody);
    if (!node.empty()) {
        setParameters(node, kin);
        setRate(newReactionRate(node, calculateRateCoeffUnits(kin)));
    } else {
        setRate(newReactionRate(type()));
    }
}

bool ThreeBodyReaction::detectEfficiencies()
{
    for (const auto& reac : reactants) {
        // detect explicitly specified collision partner
        if (products.count(reac.first)) {
            m_third_body->efficiencies[reac.first] = 1.;
        }
    }

    if (m_third_body->efficiencies.size() == 0) {
        return false;
    } else if (m_third_body->efficiencies.size() > 1) {
        throw CanteraError("ThreeBodyReaction::detectEfficiencies",
            "Found more than one explicitly specified collision partner\n"
            "in reaction '{}'.", equation());
    }

    m_third_body->default_efficiency = 0.;
    m_third_body->specified_collision_partner = true;
    auto sp = m_third_body->efficiencies.begin();

    // adjust reactant coefficients
    auto reac = reactants.find(sp->first);
    if (trunc(reac->second) != 1) {
        reac->second -= 1.;
    } else {
        reactants.erase(reac);
    }

    // adjust product coefficients
    auto prod = products.find(sp->first);
    if (trunc(prod->second) != 1) {
        prod->second -= 1.;
    } else {
        products.erase(prod);
    }

    return true;
}

void ThreeBodyReaction::setEquation(const std::string& equation, const Kinetics* kin)
{
    Reaction::setEquation(equation, kin);
    if (reactants.count("M") != 1 || products.count("M") != 1) {
        if (!detectEfficiencies()) {
            throw InputFileError("ThreeBodyReaction::setParameters", input,
                "Reaction equation '{}' does not contain third body 'M'",
                equation);
        }
        return;
    }

    reactants.erase("M");
    products.erase("M");
}

void ThreeBodyReaction::setParameters(const AnyMap& node, const Kinetics& kin)
{
    if (node.empty()) {
        // empty node: used by newReaction() factory loader
        return;
    }
    Reaction::setParameters(node, kin);
    if (!m_third_body->specified_collision_partner) {
        m_third_body->setEfficiencies(node);
    }
}

void ThreeBodyReaction::getParameters(AnyMap& reactionNode) const
{
    Reaction::getParameters(reactionNode);
    if (!m_third_body->specified_collision_partner) {
        reactionNode["type"] = "three-body";
        reactionNode["efficiencies"] = m_third_body->efficiencies;
        reactionNode["efficiencies"].setFlowStyle();
        if (m_third_body->default_efficiency != 1.0) {
            reactionNode["default-efficiency"] = m_third_body->default_efficiency;
        }
    }
}

std::string ThreeBodyReaction::reactantString() const
{
    if (m_third_body->specified_collision_partner) {
        return Reaction::reactantString() + " + "
            + m_third_body->efficiencies.begin()->first;
    } else {
        return Reaction::reactantString() + " + M";
    }
}

std::string ThreeBodyReaction::productString() const
{
    if (m_third_body->specified_collision_partner) {
        return Reaction::productString() + " + "
            + m_third_body->efficiencies.begin()->first;
    } else {
        return Reaction::productString() + " + M";
    }
}

FalloffReaction::FalloffReaction()
    : Reaction()
{
    m_third_body.reset(new ThirdBody);
    m_third_body->mass_action = false;
    setRate(newReactionRate(type()));
}

FalloffReaction::FalloffReaction(const Composition& reactants,
                                 const Composition& products,
                                 const ReactionRate& rate,
                                 const ThirdBody& tbody)
    : Reaction(reactants, products)
{
    m_third_body = std::make_shared<ThirdBody>(tbody);
    m_third_body->mass_action = false;
    AnyMap node = rate.parameters();
    node.applyUnits();
    std::string rate_type = node["type"].asString();
    if (rate_type != "falloff" && rate_type != "chemically-activated") {
        // use node information to determine whether rate is a falloff rate
        throw CanteraError("FalloffReaction::FalloffReaction",
            "Incompatible types: '{}' is not a falloff rate object.", rate.type());
    }
    setRate(newReactionRate(node));
}

FalloffReaction::FalloffReaction(const AnyMap& node, const Kinetics& kin)
{
    m_third_body.reset(new ThirdBody);
    m_third_body->mass_action = false;
    if (!node.empty()) {
        setParameters(node, kin);
        setRate(newReactionRate(node, calculateRateCoeffUnits(kin)));
    } else {
        setRate(newReactionRate(type()));
    }
}

std::string FalloffReaction::type() const
{
    if (m_rate &&
        std::dynamic_pointer_cast<FalloffRate>(m_rate)->chemicallyActivated())
    {
        return "chemically-activated";
    }
    return "falloff";
}

std::string FalloffReaction::reactantString() const
{
    if (m_third_body->specified_collision_partner) {
        return Reaction::reactantString() + " (+" +
            m_third_body->efficiencies.begin()->first + ")";
    } else {
        return Reaction::reactantString() + " (+M)";
    }
}

std::string FalloffReaction::productString() const
{
    if (m_third_body->specified_collision_partner) {
        return Reaction::productString() + " (+" +
            m_third_body->efficiencies.begin()->first + ")";
    } else {
        return Reaction::productString() + " (+M)";
    }
}

void FalloffReaction::setParameters(const AnyMap& node, const Kinetics& kin)
{
    if (node.empty()) {
        // empty node: used by newReaction() factory loader
        return;
    }
    Reaction::setParameters(node, kin);

    if (!m_third_body->specified_collision_partner) {
        m_third_body->setEfficiencies(node);
    }
}

void FalloffReaction::setEquation(const std::string& equation, const Kinetics* kin)
{
    Reaction::setEquation(equation, kin);

    // Detect falloff third body based on partial setup;
    // parseReactionEquation (called via Reaction::setEquation) sets the
    // stoichiometric coefficient of the falloff species to -1.
    std::string third_body_str;
    std::string third_body;
    for (auto& reactant : reactants) {
        if (reactant.second == -1 && ba::starts_with(reactant.first, "(+")) {
            third_body_str = reactant.first;
            third_body = third_body_str.substr(2, third_body_str.size() - 3);
            break;
        }
    }

    // Equation must contain a third body, and it must appear on both sides
    if (third_body_str == "") {
        throw InputFileError("FalloffReaction::setParameters", input,
            "Reactants for reaction '{}' do not contain a pressure-dependent "
            "third body", equation);
    }
    if (products.count(third_body_str) == 0) {
        throw InputFileError("FalloffReaction::setParameters", input,
            "Unable to match third body '{}' in reactants and products of "
            "reaction '{}'", third_body, equation);
    }

    // Remove the dummy species
    reactants.erase(third_body_str);
    products.erase(third_body_str);

    if (third_body == "M") {
        m_third_body->specified_collision_partner = false;
    } else {
        // Specific species is listed as the third body
        m_third_body->default_efficiency = 0;
        m_third_body->efficiencies.emplace(third_body, 1.0);
        m_third_body->specified_collision_partner = true;
    }
}

void FalloffReaction::getParameters(AnyMap& reactionNode) const
{
    Reaction::getParameters(reactionNode);
    if (m_third_body->specified_collision_partner) {
        // pass
    } else if (m_third_body->efficiencies.size()) {
        reactionNode["efficiencies"] = m_third_body->efficiencies;
        reactionNode["efficiencies"].setFlowStyle();
        if (m_third_body->default_efficiency != 1.0) {
            reactionNode["default-efficiency"] = m_third_body->default_efficiency;
        }
    }
}

CustomFunc1Reaction::CustomFunc1Reaction()
{
    setRate(newReactionRate(type()));
}

CustomFunc1Reaction::CustomFunc1Reaction(const Composition& reactants,
                                         const Composition& products,
                                         const CustomFunc1Rate& rate)
    : Reaction(reactants, products)
{
    m_rate.reset(new CustomFunc1Rate(rate));
}

CustomFunc1Reaction::CustomFunc1Reaction(const AnyMap& node, const Kinetics& kin)
{
    if (!node.empty()) {
        setParameters(node, kin);
        setRate(newReactionRate(node, calculateRateCoeffUnits(kin)));
    } else {
        setRate(newReactionRate(type()));
    }
}

bool isThreeBody(const Reaction& R)
{
    // detect explicitly specified collision partner
    size_t found = 0;
    for (const auto& reac : R.reactants) {
        auto prod = R.products.find(reac.first);
        if (prod != R.products.end() &&
            trunc(reac.second) == reac.second && trunc(prod->second) == prod->second) {
            // candidate species with integer stoichiometric coefficients on both sides
            found += 1;
        }
    }
    if (found != 1) {
        return false;
    }

    // ensure that all reactants have integer stoichiometric coefficients
    size_t nreac = 0;
    for (const auto& reac : R.reactants) {
       if (trunc(reac.second) != reac.second) {
           return false;
       }
       nreac += static_cast<size_t>(reac.second);
    }

    // ensure that all products have integer stoichiometric coefficients
    size_t nprod = 0;
    for (const auto& prod : R.products) {
       if (trunc(prod.second) != prod.second) {
           return false;
       }
       nprod += static_cast<size_t>(prod.second);
    }

    // either reactant or product side involves exactly three species
    return (nreac == 3) || (nprod == 3);
}

unique_ptr<Reaction> newReaction(const std::string& type)
{
    AnyMap rxn_node;
    Kinetics kin;
    unique_ptr<Reaction> R(ReactionFactory::factory()->create(type, rxn_node, kin));
    return R;
}

unique_ptr<Reaction> newReaction(const AnyMap& rxn_node, const Kinetics& kin)
{
    std::string type = "elementary";
    size_t nDim = kin.thermo(kin.reactionPhaseIndex()).nDim();
    if (rxn_node.hasKey("type")) {
        type = rxn_node["type"].asString();
    } else if (nDim == 3) {
        // Reaction type is not specified
        // See if this is a three-body reaction with a specified collision partner
        Reaction testReaction;
        parseReactionEquation(testReaction, rxn_node["equation"].asString(),
                              rxn_node, &kin);
        if (isThreeBody(testReaction)) {
            type = "three-body";
        }
    }

    if (!(ReactionFactory::factory()->exists(type))) {
        throw InputFileError("ReactionFactory::newReaction", rxn_node["type"],
            "Unknown reaction type '{}'", type);
    }
    Reaction* R = ReactionFactory::factory()->create(type, rxn_node, kin);
    return unique_ptr<Reaction>(R);
}

void parseReactionEquation(Reaction& R, const std::string& equation,
                           const AnyBase& reactionNode, const Kinetics* kin)
{
    // Parse the reaction equation to determine participating species and
    // stoichiometric coefficients
    std::vector<std::string> tokens;
    tokenizeString(equation, tokens);
    tokens.push_back("+"); // makes parsing last species not a special case

    size_t last_used = npos; // index of last-used token
    bool reactants = true;
    for (size_t i = 1; i < tokens.size(); i++) {
        if (tokens[i] == "+" || ba::starts_with(tokens[i], "(+") ||
            tokens[i] == "<=>" || tokens[i] == "=" || tokens[i] == "=>") {
            std::string species = tokens[i-1];

            double stoich;
            if (last_used != npos && tokens[last_used] == "(+") {
                // Falloff third body with space, such as "(+ M)"
                species = "(+" + species;
                stoich = -1;
            } else if (last_used == i-1 && ba::starts_with(species, "(+")
                       && ba::ends_with(species, ")")) {
                // Falloff 3rd body written without space, such as "(+M)"
                stoich = -1;
            } else if (last_used == i-2) { // Species with no stoich. coefficient
                stoich = 1.0;
            } else if (last_used == i-3) { // Stoich. coefficient and species
                try {
                    stoich = fpValueCheck(tokens[i-2]);
                } catch (CanteraError& err) {
                    throw InputFileError("parseReactionEquation", reactionNode,
                        err.getMessage());
                }
            } else {
                throw InputFileError("parseReactionEquation", reactionNode,
                    "Error parsing reaction string '{}'.\n"
                    "Current token: '{}'\nlast_used: '{}'",
                    equation, tokens[i],
                    (last_used == npos) ? "n/a" : tokens[last_used]);
            }
            if (!kin || (kin->kineticsSpeciesIndex(species) == npos
                         && stoich != -1 && species != "M"))
            {
                R.setValid(false);
            }

            if (reactants) {
                R.reactants[species] += stoich;
            } else {
                R.products[species] += stoich;
            }

            last_used = i;
        }

        // Tokens after this point are part of the products string
        if (tokens[i] == "<=>" || tokens[i] == "=") {
            R.reversible = true;
            reactants = false;
        } else if (tokens[i] == "=>") {
            R.reversible = false;
            reactants = false;
        }
    }
}

std::vector<shared_ptr<Reaction>> getReactions(const AnyValue& items,
                                               Kinetics& kinetics)
{
    std::vector<shared_ptr<Reaction>> all_reactions;
    for (const auto& node : items.asVector<AnyMap>()) {
        shared_ptr<Reaction> R(newReaction(node, kinetics));
        R->check();
        R->validate(kinetics);
        if (R->valid() && R->checkSpecies(kinetics)) {
            all_reactions.emplace_back(R);
        }
    }
    return all_reactions;
}

}
