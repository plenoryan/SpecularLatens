document.addEventListener('DOMContentLoaded', () => {
    const sliderContainer = document.querySelector('.slider-container');
    const sliderHandle = document.querySelector('.slider-handle');
    const imageBefore = document.querySelector('.slider-image-before');
    
    let isDragging = false;

    // Set initial position to 50%
    let currentPercentage = 50;
    
    function updateSliderPosition(clientX) {
        const rect = sliderContainer.getBoundingClientRect();
        
        // Calculate position inside container
        let x = clientX - rect.left;
        
        // Constrain to container boundaries
        if (x < 0) x = 0;
        if (x > rect.width) x = rect.width;
        
        // Convert to percentage
        currentPercentage = (x / rect.width) * 100;
        
        // Apply position
        sliderHandle.style.left = `${currentPercentage}%`;
        
        // Update the clip-path of the top image
        // inset(top right bottom left)
        imageBefore.style.clipPath = `inset(0 ${100 - currentPercentage}% 0 0)`;
    }

    // Mouse events
    sliderContainer.addEventListener('mousedown', (e) => {
        isDragging = true;
        updateSliderPosition(e.clientX);
    });

    window.addEventListener('mousemove', (e) => {
        if (!isDragging) return;
        updateSliderPosition(e.clientX);
    });

    window.addEventListener('mouseup', () => {
        isDragging = false;
    });

    // Touch events for mobile
    sliderContainer.addEventListener('touchstart', (e) => {
        isDragging = true;
        updateSliderPosition(e.touches[0].clientX);
    });

    window.addEventListener('touchmove', (e) => {
        if (!isDragging) return;
        // Prevent scrolling while sliding
        e.preventDefault(); 
        updateSliderPosition(e.touches[0].clientX);
    }, { passive: false });

    window.addEventListener('touchend', () => {
        isDragging = false;
    });
});
