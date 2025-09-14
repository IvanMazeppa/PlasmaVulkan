#!/usr/bin/env python3
"""
Automated Job Watcher for PlasmaVulkan RT Development
Monitors changes folder and triggers Claude API for automated task execution
"""

import json
import time
import os
from pathlib import Path
from datetime import datetime
from typing import Dict, Any, Optional, List
import hashlib
from dataclasses import dataclass, asdict
from enum import Enum

# You'll need to install: pip install anthropic watchdog
from anthropic import Anthropic
from watchdog.observers import Observer
from watchdog.events import FileSystemEventHandler, FileCreatedEvent

@dataclass
class JobConfig:
    """Configuration for job processing"""
    changes_dir: Path = Path("D:/Users/dilli/AndroidStudioProjects/PlasmaVulkan/changes")
    results_dir: Path = Path("D:/Users/dilli/AndroidStudioProjects/PlasmaVulkan/results")
    processed_dir: Path = Path("D:/Users/dilli/AndroidStudioProjects/PlasmaVulkan/changes/processed")
    logs_dir: Path = Path("D:/Users/dilli/AndroidStudioProjects/PlasmaVulkan/automation/logs")
    
    # Processing settings
    auto_process: bool = True
    require_confirmation: bool = False  # Set to True for manual review before processing
    max_retries: int = 3
    retry_delay: float = 5.0
    
    # Claude settings
    model: str = "claude-3-5-sonnet-20241022"  # or claude-3-opus-20240229
    max_tokens: int = 8000
    temperature: float = 0.3  # Lower for more consistent technical work
    
    def __post_init__(self):
        # Ensure directories exist
        for dir_path in [self.processed_dir, self.logs_dir]:
            dir_path.mkdir(parents=True, exist_ok=True)

class JobStatus(Enum):
    PENDING = "pending"
    PROCESSING = "processing"
    COMPLETED = "completed"
    FAILED = "failed"
    REQUIRES_REVIEW = "requires_review"

class JobProcessor:
    """Processes jobs from GPT-5 using Claude API"""
    
    def __init__(self, config: JobConfig, api_key: Optional[str] = None):
        self.config = config
        self.client = Anthropic(api_key=api_key or os.environ.get("ANTHROPIC_API_KEY"))
        self.processed_jobs: Dict[str, str] = self._load_processed_jobs()
        
    def _load_processed_jobs(self) -> Dict[str, str]:
        """Load history of processed jobs to avoid duplicates"""
        history_file = self.config.logs_dir / "processed_history.json"
        if history_file.exists():
            with open(history_file, 'r') as f:
                return json.load(f)
        return {}
    
    def _save_processed_job(self, job_id: str, status: str):
        """Save processed job to history"""
        self.processed_jobs[job_id] = status
        history_file = self.config.logs_dir / "processed_history.json"
        with open(history_file, 'w') as f:
            json.dump(self.processed_jobs, f, indent=2)
    
    def _generate_claude_prompt(self, job: Dict[str, Any]) -> str:
        """Generate a comprehensive prompt for Claude"""
        return f"""You are assisting with implementing ray tracing in a Vulkan 1.4 particle simulation project.

## Job Details:
**ID**: {job.get('id', 'unknown')}
**Title**: {job.get('title', 'Untitled task')}
**Purpose**: {job.get('purpose', 'No purpose specified')}

## Implementation Steps:
{json.dumps(job.get('steps', []), indent=2)}

## Acceptance Criteria:
{json.dumps(job.get('acceptance', []), indent=2)}

## Files to Modify:
{json.dumps(job.get('touchpoints', []), indent=2)}

## MCP Queries to Run:
{json.dumps(job.get('mcp_queries', []), indent=2)}

## Additional Notes:
{job.get('notes', 'None')}

Please:
1. Analyze the requirements carefully
2. Use the Vulkan documentation MCP server for any API queries listed
3. Read the current implementation from the specified files
4. Implement the changes as described
5. Validate against the acceptance criteria
6. Generate a detailed result JSON following the established schema

Return your response as a JSON object with the result structure.
"""
    
    def process_job(self, job_file: Path) -> Optional[Dict[str, Any]]:
        """Process a single job file"""
        try:
            # Read job
            with open(job_file, 'r') as f:
                job = json.load(f)
            
            job_id = job.get('id', job_file.stem)
            
            # Check if already processed
            if job_id in self.processed_jobs:
                print(f"Job {job_id} already processed with status: {self.processed_jobs[job_id]}")
                return None
            
            print(f"\n{'='*60}")
            print(f"Processing Job: {job_id}")
            print(f"Title: {job.get('title', 'Untitled')}")
            print(f"{'='*60}")
            
            # Generate prompt
            prompt = self._generate_claude_prompt(job)
            
            # Call Claude API
            response = self.client.messages.create(
                model=self.config.model,
                max_tokens=self.config.max_tokens,
                temperature=self.config.temperature,
                messages=[
                    {
                        "role": "user",
                        "content": prompt
                    }
                ]
            )
            
            # Parse response
            result_text = response.content[0].text
            
            # Try to extract JSON from response
            result = self._extract_json_result(result_text, job_id)
            
            # Save result
            result_file = self.config.results_dir / f"{job_id}_result.json"
            with open(result_file, 'w') as f:
                json.dump(result, f, indent=2)
            
            print(f"✅ Job {job_id} completed successfully")
            print(f"   Result saved to: {result_file}")
            
            # Move processed job
            processed_file = self.config.processed_dir / job_file.name
            job_file.rename(processed_file)
            
            # Update history
            self._save_processed_job(job_id, JobStatus.COMPLETED.value)
            
            return result
            
        except Exception as e:
            print(f"❌ Error processing job {job_file.name}: {e}")
            self._save_processed_job(job_id, JobStatus.FAILED.value)
            
            # Log error
            error_log = self.config.logs_dir / f"error_{job_id}_{datetime.now().strftime('%Y%m%d_%H%M%S')}.txt"
            with open(error_log, 'w') as f:
                f.write(f"Error processing job {job_id}\n")
                f.write(f"Error: {str(e)}\n")
                f.write(f"Job file: {job_file}\n")
            
            return None
    
    def _extract_json_result(self, text: str, job_id: str) -> Dict[str, Any]:
        """Extract JSON from Claude's response"""
        try:
            # Try to find JSON in response
            import re
            json_match = re.search(r'\{.*\}', text, re.DOTALL)
            if json_match:
                return json.loads(json_match.group())
            else:
                # Fallback: create basic result structure
                return {
                    "job_id": job_id,
                    "status": "completed",
                    "timestamp": datetime.now().isoformat(),
                    "raw_response": text,
                    "notes": "Could not parse structured JSON from response"
                }
        except:
            return {
                "job_id": job_id,
                "status": "parse_error",
                "timestamp": datetime.now().isoformat(),
                "raw_response": text
            }

class JobWatcher(FileSystemEventHandler):
    """Watches for new job files and triggers processing"""
    
    def __init__(self, processor: JobProcessor):
        self.processor = processor
        self.processing = set()  # Track files being processed
        
    def on_created(self, event):
        if isinstance(event, FileCreatedEvent) and event.src_path.endswith('.json'):
            # Small delay to ensure file is fully written
            time.sleep(0.5)
            
            file_path = Path(event.src_path)
            if file_path.name not in self.processing:
                self.processing.add(file_path.name)
                print(f"\n🔔 New job detected: {file_path.name}")
                
                if self.processor.config.require_confirmation:
                    response = input("Process this job? (y/n): ")
                    if response.lower() != 'y':
                