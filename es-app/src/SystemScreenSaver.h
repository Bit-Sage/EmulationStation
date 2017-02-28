#pragma once

#include "Window.h"

class VideoComponent;

std::string	getTitlePath();

// Screensaver implementation for main window
class SystemScreenSaver : public Window::ScreenSaver
{
public:
	SystemScreenSaver(Window* window);
	virtual ~SystemScreenSaver();

	virtual void startScreenSaver();
	virtual void stopScreenSaver();
	virtual void renderScreenSaver();
	virtual bool allowSleep();
	virtual void update(int deltaTime);

private:
	void	countVideos();
	void	pickRandomVideo(std::string& path);
    void	writeSubtitle();

    // WIP

    virtual const char* getSystemName();
	virtual const char* getGameName();
	virtual int getGameIndex();
	void input(InputConfig* config, Input input);

	enum STATE {
		STATE_INACTIVE,
		STATE_FADE_OUT_WINDOW,
		STATE_FADE_IN_VIDEO,
		STATE_SCREENSAVER_ACTIVE
	};

private:
	bool			mCounted;
	unsigned long	mVideoCount;
	VideoComponent* mVideoScreensaver;
	Window*			mWindow;
	STATE			mState;
	float			mOpacity;
	int				mTimer;
	int   			mGameIndex;
	const char*		mGameName;
	const char*		mSystemName;	
};
