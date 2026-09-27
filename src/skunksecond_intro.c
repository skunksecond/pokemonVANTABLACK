#include "global.h"
#include "main.h"
#include "task.h"
#include "sound.h"
#include "constants/songs.h"
#include "task.h"
#include "video.h"
#include "intro.h"
#include "m4a.h"

// Expose the global engine pointers used by video.c
extern s32 GetVideoCurrentFrame(void);

// Local state machine tracker
static u8 sCustomLogoState = 0;
static u8 sPauseTimelineState = 0;



// TODO: fix this stupid ass ai code that sucks and doesn't work fuck ai fuck vibecoding fuck google
void CB2_CustomLogoSequence(void)
{
    return;

    u8 videoTaskId;
    
    // Find the task ID dynamically by looping through active tasks
    for (videoTaskId = 0; videoTaskId < NUM_TASKS; videoTaskId++)
    {
        if (gTasks[videoTaskId].isActive && !VideoPlayer_IsDone())
        {
            // If the video player is running, this is our task ID!
            break;
        }
    }

    switch (sCustomLogoState)
    {
    case 0:
        ResetTasks();
        sPauseTimelineState = 0; // Clear tracking registers
        if (VideoPlayer_Start() != TASK_NONE) 
            sCustomLogoState++;
        break;

    case 1:
        // Double-check that we have a valid, running video player task index
        if (videoTaskId < NUM_TASKS)
        {
            // TIMELINE INTERCEPT: Hit frame 45? 
            if (GetVideoCurrentFrame() == 37)
            {
                // Forcefully clear tShouldCopy (data[3] in video.c) to lock VRAM in place
                gTasks[videoTaskId].data[3] = FALSE; 
                
                switch (sPauseTimelineState)
                {
                case 0:
                    m4aSoundVSyncOn();            // Reactivate Sappy audio clocks
                    PlaySE(SEQ_SKUNKSECOND_LOGO); // Fire the logo sound theme
                    sPauseTimelineState++;
                    break;
                    
                case 1:
                    if (!IsSEPlaying())
                    {
                        PlaySE(SE_SONIC3_EXPLOSION_SFX); // Trigger explosion sound
                        sPauseTimelineState++;
                    }
                    break;
                    
                case 2:
                    if (!IsSEPlaying())
                    {
                        m4aSoundVSyncOff();          // Safely shut down Sappy audio registers
                        sCustomLogoState++;          // Advance main machine state forward
                    }
                    break;
                }
                
                // Bypass the standard RunTasks frame processing call while frozen
                break; 
            }
        }
        
        // If not frame 45, process video decoding frames at normal speed
        RunTasks();
        break;

    case 2:
        // Unfreeze: Allow background decoding tasks to resume calculations
        RunTasks();
        
        if (VideoPlayer_IsDone())
        {
            sCustomLogoState = 0; // Clear locally for hard/soft system resets
            m4aSoundVSyncOn();    // Re-enable sound permanently for the main game intro
            
            // Reassign global engine state sequence to safely route to Title screen intro assets
            gMain.state = 4; // Targets COPYRIGHT_START_INTRO sequence triggers

#if EXPANSION_INTRO == TRUE
            SetMainCallback2(CB2_ExpansionIntro);
            CreateTask(Task_HandleExpansionIntro, 0);
#else
            CreateTask(Task_Scene1_Load, 0);
            SetMainCallback2(MainCB2_Intro);
#endif
        }
        break;
    }
}
