/*
 * Living Azeroth prototypes.
 */

void AddLivingAzerothWarScripts();
void AddLivingAzerothPopulationScripts();
void AddLivingAzerothProfileScripts();
void AddLivingAzerothDirectorScripts();

void Addmod_living_azeroth_warScripts()
{
    // Register the independent legacy controllers and the AI3 observe-only director.
    AddLivingAzerothWarScripts();
    AddLivingAzerothPopulationScripts();
    AddLivingAzerothProfileScripts();
    AddLivingAzerothDirectorScripts();
}
