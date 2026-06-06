#ifndef TYPES_HPP
#define TYPES_HPP

#include <string>
#include <vector>

// Enumerated types for fast integer-based game loop execution
enum class DieFace { MIGHT, CUNNING, WISDOM, SHIELD, BLANK };

enum class CardType {
  NARRATIVE_EVENT,
  STANDARD_COMBAT,
  BRANCHING_CHOICE,
  SKILL_CHALLENGE,
  BOSS_BATTLE
};

struct Prisoner {
  int id = 0;  // Primary key tracking identifier
  std::string name;
  int current_hp = 0;
  int max_hp = 0;
  std::vector<DieFace>
      dice_faces;  // Swapped to high-speed enums for logic processing
};

struct ItemCard {
  int id = 0;
  std::string name;
  std::string description;

  bool is_2handed = false;
  bool is_consumable = false;
  bool is_weapon = false;
  bool is_immunity_shield = false;

  int attack_amount = 0;
  int heal_amount = 0;

  // Moving item mechanics to data fields instead of C++ source files
  std::string special_action_type;  // e.g., "REROLL_CUNNING", "REROLL_BLANK",
                                    // "PASSIVE_ARMOR"
};

struct ChapterCard {
  int id = 0;  // Primary key tracking identifier
  std::string title;
  std::string flavor_text;

  // Unified classification to completely eliminate state overlaps
  CardType type = CardType::NARRATIVE_EVENT;

  // Standard Combat parameters
  int attack_damage = 2;
  std::vector<DieFace> enemy_shields;

  // Branching Pre-Combat Choice Parameters
  std::string choice_1_text;
  int choice_1_damage = 0;
  std::vector<DieFace> choice_1_shields;  // Structured vector arrays instead of
                                          // messy raw text blobs

  std::string choice_2_text;
  int choice_2_damage = 0;
  std::vector<DieFace> choice_2_shields;

  // Challenge & Trap Parameters
  DieFace target_attribute = DieFace::BLANK;
  int required_successes = 0;
  int max_attempts = 0;
  int damage_per_roll = 0;
  int failure_damage = 0;

  // Loot Drop System Hooks
  bool has_reward = false;
  std::string reward_type;  // e.g., "ITEM", "HEAL"
  int reward_amount = 0;
  int trap_damage = 0;
};

#endif
